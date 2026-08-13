#include "fh6r/camera_re_probe.hpp"

#include "fh6r/log.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

namespace fh6r {
namespace {

constexpr std::uint32_t kExpectedTimestamp = 0x6A58A086u;
constexpr std::uint32_t kExpectedChecksum = 0x0AF21AAAu;
constexpr std::uintptr_t kCabinViewFlagRva = 0x08EF67C0u;
constexpr std::uint64_t kWrite4ControlNibble = 0xDu; // RW=01 (write), LEN=11 (4 bytes)

CameraReProbe* volatile g_active_probe = nullptr;

bool target_is_in_data(const fmod::PEImage& image, std::uintptr_t target) noexcept {
    const auto* ptr = reinterpret_cast<const std::byte*>(target);
    for (const auto& section : image.sections) {
        if (std::strncmp(section.name.data(), ".data", 8) != 0) continue;
        return ptr >= section.start && ptr + sizeof(std::uint32_t) <= section.end;
    }
    return false;
}

std::string hex_bytes(const fmod::PEImage& image, std::uintptr_t center,
                      std::uintptr_t& start_rva) {
    start_rva = 0;
    const auto text_begin = reinterpret_cast<std::uintptr_t>(image.text);
    const auto text_end = reinterpret_cast<std::uintptr_t>(image.text_end);
    if (center < text_begin || center >= text_end) return {};

    constexpr std::size_t kBefore = 32;
    constexpr std::size_t kAfter = 32;
    const auto start = std::max(text_begin, center > kBefore ? center - kBefore : text_begin);
    const auto end = std::min(text_end, center + kAfter);
    if (end <= start) return {};

    std::array<std::uint8_t, kBefore + kAfter> bytes{};
    SIZE_T read = 0;
    const SIZE_T wanted = static_cast<SIZE_T>(std::min<std::uintptr_t>(bytes.size(), end - start));
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(start),
                           bytes.data(), wanted, &read) || read == 0) {
        return {};
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(static_cast<std::size_t>(read) * 3);
    for (SIZE_T i = 0; i < read; ++i) {
        const auto b = bytes[static_cast<std::size_t>(i)];
        if (i) out.push_back(' ');
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    start_rva = start - reinterpret_cast<std::uintptr_t>(image.base);
    return out;
}

std::uint32_t containing_function_rva(const fmod::PEImage& image,
                                      std::uintptr_t absolute_address) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(image.base);
    if (absolute_address < base || absolute_address >= base + image.size) return 0;
    const auto rva64 = absolute_address - base;
    if (rva64 > UINT32_MAX) return 0;
    const auto rva = static_cast<std::uint32_t>(rva64);
    const auto it = std::upper_bound(image.function_rvas.begin(), image.function_rvas.end(), rva);
    if (it == image.function_rvas.begin()) return 0;
    return *std::prev(it);
}

class ScopedSuspend {
public:
    explicit ScopedSuspend(HANDLE thread) noexcept : thread_(thread) {
        suspended_ = SuspendThread(thread_) != static_cast<DWORD>(-1);
    }
    ~ScopedSuspend() {
        if (suspended_) ResumeThread(thread_);
    }
    bool ok() const noexcept { return suspended_; }
private:
    HANDLE thread_ = nullptr;
    bool suspended_ = false;
};

} // namespace

CameraReProbe::CameraReProbe(const fmod::PEImage& image) noexcept : image_(&image) {
    if (!image.valid()) {
        log::warn("[camera-re] disabled: invalid PE image");
        return;
    }
    if (image.time_date_stamp != kExpectedTimestamp || image.checksum != kExpectedChecksum) {
        log::warn("[camera-re] disabled: build mismatch timestamp=0x{:08X} checksum=0x{:08X}",
                  image.time_date_stamp, image.checksum);
        return;
    }
    if (kCabinViewFlagRva + sizeof(std::uint32_t) > image.size) {
        log::warn("[camera-re] disabled: target RVA outside image");
        return;
    }

    target_ = reinterpret_cast<std::uintptr_t>(image.base) + kCabinViewFlagRva;
    if (!target_is_in_data(image, target_)) {
        log::warn("[camera-re] disabled: +0x{:X} is not inside .data", kCabinViewFlagRva);
        target_ = 0;
        return;
    }
    if (IsDebuggerPresent()) {
        log::warn("[camera-re] disabled: debugger already attached; refusing to alter DR0-DR3");
        target_ = 0;
        return;
    }

    const auto initial = *reinterpret_cast<volatile const std::uint32_t*>(target_);
    InterlockedExchange(&last_value_, static_cast<LONG>(initial));

    if (InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_active_probe), this, nullptr) != nullptr) {
        log::warn("[camera-re] disabled: another probe instance is already active");
        target_ = 0;
        return;
    }

    veh_handle_ = AddVectoredExceptionHandler(1, &CameraReProbe::vectored_handler);
    if (!veh_handle_) {
        InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_active_probe), nullptr, this);
        log::warn("[camera-re] disabled: AddVectoredExceptionHandler failed error={}", GetLastError());
        target_ = 0;
        return;
    }

    InterlockedExchange(&enabled_, 1);
    try {
        worker_ = std::jthread([this](std::stop_token stop) { worker(stop); });
    } catch (...) {
        InterlockedExchange(&enabled_, 0);
        RemoveVectoredExceptionHandler(veh_handle_);
        veh_handle_ = nullptr;
        InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_active_probe), nullptr, this);
        target_ = 0;
        log::warn("[camera-re] disabled: worker thread creation failed");
        return;
    }
    log::info("[camera-re] active target=+0x{:X} absolute=0x{:X} initial={} build={:08X}/{:08X}",
              kCabinViewFlagRva, target_, initial, image.time_date_stamp, image.checksum);
}

CameraReProbe::~CameraReProbe() {
    if (worker_.joinable()) {
        worker_.request_stop();
        try { worker_.join(); } catch (...) {}
    }
    InterlockedExchange(&enabled_, 0);
    if (veh_handle_) {
        RemoveVectoredExceptionHandler(veh_handle_);
        veh_handle_ = nullptr;
    }
    InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_active_probe), nullptr, this);
}

LONG CALLBACK CameraReProbe::vectored_handler(EXCEPTION_POINTERS* pointers) noexcept {
    auto* probe = static_cast<CameraReProbe*>(InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_active_probe), nullptr, nullptr));
    if (!probe) return EXCEPTION_CONTINUE_SEARCH;
    return probe->handle_exception(pointers);
}

std::uintptr_t CameraReProbe::debug_register_value(const CONTEXT& ctx, unsigned slot) const noexcept {
    switch (slot) {
        case 0: return static_cast<std::uintptr_t>(ctx.Dr0);
        case 1: return static_cast<std::uintptr_t>(ctx.Dr1);
        case 2: return static_cast<std::uintptr_t>(ctx.Dr2);
        case 3: return static_cast<std::uintptr_t>(ctx.Dr3);
        default: return 0;
    }
}

void CameraReProbe::set_debug_register_value(CONTEXT& ctx, unsigned slot,
                                              std::uintptr_t value) noexcept {
    switch (slot) {
        case 0: ctx.Dr0 = value; break;
        case 1: ctx.Dr1 = value; break;
        case 2: ctx.Dr2 = value; break;
        case 3: ctx.Dr3 = value; break;
        default: break;
    }
}

LONG CameraReProbe::handle_exception(EXCEPTION_POINTERS* pointers) noexcept {
    if (InterlockedCompareExchange(&enabled_, 0, 0) == 0 || !pointers ||
        !pointers->ExceptionRecord || !pointers->ContextRecord ||
        pointers->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT& ctx = *pointers->ContextRecord;
    unsigned hit_slot = 4;
    for (unsigned slot = 0; slot < 4; ++slot) {
        const auto status_bit = std::uint64_t{1} << slot;
        const auto local_enable = std::uint64_t{1} << (slot * 2);
        const auto control_shift = 16u + slot * 4u;
        const auto control = (static_cast<std::uint64_t>(ctx.Dr7) >> control_shift) & 0xFu;
        if ((static_cast<std::uint64_t>(ctx.Dr6) & status_bit) != 0 &&
            (static_cast<std::uint64_t>(ctx.Dr7) & local_enable) != 0 &&
            control == kWrite4ControlNibble && debug_register_value(ctx, slot) == target_) {
            hit_slot = slot;
            break;
        }
    }
    if (hit_slot >= 4) return EXCEPTION_CONTINUE_SEARCH;

    const auto current = *reinterpret_cast<volatile const std::uint32_t*>(target_);
    const auto previous = static_cast<std::uint32_t>(
        InterlockedExchange(&last_value_, static_cast<LONG>(current)));

    // The data breakpoint traps after the writing instruction. Preserve the trap
    // RIP and full integer register state; offline analysis will decode backward
    // from trap_rip to identify the exact writer instruction.
    if (current != previous) {
        const LONG index = InterlockedIncrement(&next_event_) - 1;
        if (index >= 0 && static_cast<std::size_t>(index) < events_.size()) {
            auto& slot = events_[static_cast<std::size_t>(index)];
            auto& event = slot.event;
            event.thread_id = GetCurrentThreadId();
            event.previous_value = previous;
            event.current_value = current;
            event.slot = static_cast<std::uint8_t>(hit_slot);
            event.trap_rip = static_cast<std::uintptr_t>(ctx.Rip);
            event.rflags = ctx.EFlags;
            event.dr6 = ctx.Dr6;
            event.dr7 = ctx.Dr7;
            event.regs = {{ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx,
                           ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp,
                           ctx.R8, ctx.R9, ctx.R10, ctx.R11,
                           ctx.R12, ctx.R13, ctx.R14, ctx.R15}};
            MemoryBarrier();
            InterlockedExchange(&slot.ready, 1);
        }
    }

    ctx.Dr6 &= ~(std::uint64_t{1} << hit_slot);
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool CameraReProbe::arm_thread(DWORD thread_id) noexcept {
    if (!target_ || thread_id == worker_thread_id_ || thread_id == GetCurrentThreadId()) return false;

    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                   THREAD_QUERY_INFORMATION,
                               FALSE, thread_id);
    if (!thread) return false;

    bool armed = false;
    {
        ScopedSuspend suspend{thread};
        if (suspend.ok()) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &ctx)) {
                int selected = -1;
                for (unsigned slot = 0; slot < 4; ++slot) {
                    const auto enable_mask = std::uint64_t{3} << (slot * 2);
                    const auto control_shift = 16u + slot * 4u;
                    const auto control = (static_cast<std::uint64_t>(ctx.Dr7) >> control_shift) & 0xFu;
                    if ((static_cast<std::uint64_t>(ctx.Dr7) & enable_mask) != 0 &&
                        control == kWrite4ControlNibble && debug_register_value(ctx, slot) == target_) {
                        selected = static_cast<int>(slot);
                        break;
                    }
                    if (selected < 0 && (static_cast<std::uint64_t>(ctx.Dr7) & enable_mask) == 0) {
                        selected = static_cast<int>(slot);
                    }
                }

                if (selected >= 0) {
                    const unsigned slot = static_cast<unsigned>(selected);
                    const auto enable_shift = slot * 2u;
                    const auto control_shift = 16u + slot * 4u;
                    const auto enable_mask = std::uint64_t{3} << enable_shift;
                    const auto control_mask = std::uint64_t{0xF} << control_shift;
                    auto dr7 = static_cast<std::uint64_t>(ctx.Dr7);
                    dr7 &= ~enable_mask;
                    dr7 &= ~control_mask;
                    dr7 |= std::uint64_t{1} << enable_shift; // local enable only
                    dr7 |= kWrite4ControlNibble << control_shift;
                    set_debug_register_value(ctx, slot, target_);
                    ctx.Dr6 = 0;
                    ctx.Dr7 = dr7;
                    armed = SetThreadContext(thread, &ctx) != FALSE;
                }
            }
        }
    }

    CloseHandle(thread);
    return armed;
}

void CameraReProbe::arm_new_threads() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    const DWORD process_id = GetCurrentProcessId();
    std::unordered_set<DWORD> live;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process_id) continue;
            live.insert(entry.th32ThreadID);
            if (!armed_threads_.contains(entry.th32ThreadID) &&
                entry.th32ThreadID != worker_thread_id_) {
                if (arm_thread(entry.th32ThreadID)) {
                    armed_threads_.insert(entry.th32ThreadID);
                }
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::erase_if(armed_threads_, [&](DWORD id) { return !live.contains(id); });
}

void CameraReProbe::disarm_all_threads() noexcept {
    if (!target_) return;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    const DWORD process_id = GetCurrentProcessId();
    const DWORD current_thread = GetCurrentThreadId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == current_thread) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                           THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (!thread) continue;
            {
                ScopedSuspend suspend{thread};
                if (suspend.ok()) {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(thread, &ctx)) {
                        bool changed = false;
                        auto dr7 = static_cast<std::uint64_t>(ctx.Dr7);
                        for (unsigned slot = 0; slot < 4; ++slot) {
                            const auto enable_shift = slot * 2u;
                            const auto control_shift = 16u + slot * 4u;
                            const auto enable_mask = std::uint64_t{3} << enable_shift;
                            const auto control_mask = std::uint64_t{0xF} << control_shift;
                            const auto control = (dr7 >> control_shift) & 0xFu;
                            if ((dr7 & enable_mask) != 0 && control == kWrite4ControlNibble &&
                                debug_register_value(ctx, slot) == target_) {
                                dr7 &= ~enable_mask;
                                dr7 &= ~control_mask;
                                set_debug_register_value(ctx, slot, 0);
                                changed = true;
                            }
                        }
                        if (changed) {
                            ctx.Dr6 = 0;
                            ctx.Dr7 = dr7;
                            SetThreadContext(thread, &ctx);
                        }
                    }
                }
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    armed_threads_.clear();
}

void CameraReProbe::flush_events() {
    if (!image_) return;
    while (next_flush_ < events_.size()) {
        auto& slot = events_[next_flush_];
        if (InterlockedCompareExchange(&slot.ready, 0, 0) == 0) break;

        const auto event = slot.event;
        const auto base = reinterpret_cast<std::uintptr_t>(image_->base);
        const auto trap_rva = event.trap_rip >= base ? event.trap_rip - base : 0;
        const auto function_rva = containing_function_rva(*image_, event.trap_rip);
        std::uintptr_t code_start_rva = 0;
        const auto bytes = hex_bytes(*image_, event.trap_rip, code_start_rva);

        log::info("[camera-re] HIT #{} tid={} slot=DR{} flag {}->{} trap=+0x{:X} function=+0x{:X} dr6=0x{:X} dr7=0x{:X}",
                  next_flush_ + 1, event.thread_id, event.slot,
                  event.previous_value, event.current_value, trap_rva, function_rva,
                  event.dr6, event.dr7);
        log::info("[camera-re] regs rax={:016X} rbx={:016X} rcx={:016X} rdx={:016X} rsi={:016X} rdi={:016X} rbp={:016X} rsp={:016X}",
                  event.regs[0], event.regs[1], event.regs[2], event.regs[3],
                  event.regs[4], event.regs[5], event.regs[6], event.regs[7]);
        log::info("[camera-re] regs r8={:016X} r9={:016X} r10={:016X} r11={:016X} r12={:016X} r13={:016X} r14={:016X} r15={:016X} rflags={:X}",
                  event.regs[8], event.regs[9], event.regs[10], event.regs[11],
                  event.regs[12], event.regs[13], event.regs[14], event.regs[15], event.rflags);
        if (!bytes.empty()) {
            log::info("[camera-re] code +0x{:X}: {}", code_start_rva, bytes);
        }

        InterlockedExchange(&slot.ready, 0);
        ++next_flush_;
    }
}

void CameraReProbe::worker(std::stop_token stop) noexcept {
    worker_thread_id_ = GetCurrentThreadId();
    try {
        auto next_status = std::chrono::steady_clock::now();
        while (!stop.stop_requested() && InterlockedCompareExchange(&enabled_, 0, 0) != 0) {
            arm_new_threads();
            flush_events();

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_status) {
                log::trace("[camera-re] armed_threads={} captured_transitions={}",
                           armed_threads_.size(), InterlockedCompareExchange(&next_event_, 0, 0));
                next_status = now + std::chrono::seconds(5);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch (...) {
        log::error("[camera-re] worker failed; disabling diagnostic probe");
    }

    // Keep the VEH active while removing our DR slots. The worker is the only
    // thread we deliberately never arm, so it can safely disarm every target
    // thread before the owning bridge thread unregisters the handler.
    disarm_all_threads();
    try { flush_events(); } catch (...) {}
}

} // namespace fh6r
