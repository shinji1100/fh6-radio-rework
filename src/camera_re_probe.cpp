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

namespace fh6r {
namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kExpectedTimestamp = 0x6A58A086u;
constexpr std::uint32_t kExpectedChecksum = 0x0AF21AAAu;
constexpr std::uintptr_t kCabinViewFlagRva = 0x08EF67C0u;

// DR7 R/W0 = 01b (write), LEN0 = 11b (4 bytes).
// In the four-bit R/W+LEN field at bits 16..19 this is 1101b = 0xD.
constexpr std::uint64_t kWrite4ControlNibble = 0xDu;
constexpr auto kSessionTimeout = 20s;

CameraReProbe* volatile g_active_probe = nullptr;

bool target_is_in_data(const fmod::PEImage& image, std::uintptr_t target) noexcept {
    const auto* ptr = reinterpret_cast<const std::byte*>(target);
    for (const auto& section : image.sections) {
        if (std::strncmp(section.name.data(), ".data", 8) != 0) continue;
        return ptr >= section.start && ptr + sizeof(std::uint32_t) <= section.end;
    }
    return false;
}

std::wstring make_trigger_path() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return {};
    path.resize(n);

    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    path += L"fh6-radio-rework\\camera_re_arm.txt";
    return path;
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
    const SIZE_T wanted =
        static_cast<SIZE_T>(std::min<std::uintptr_t>(bytes.size(), end - start));

    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(start),
                           bytes.data(), wanted, &read) ||
        read == 0) {
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
    const auto it =
        std::upper_bound(image.function_rvas.begin(), image.function_rvas.end(), rva);
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
        log::warn("[camera-re] dormant probe disabled: invalid PE image");
        return;
    }

    if (image.time_date_stamp != kExpectedTimestamp || image.checksum != kExpectedChecksum) {
        log::warn(
            "[camera-re] dormant probe disabled: build mismatch timestamp=0x{:08X} checksum=0x{:08X}",
            image.time_date_stamp, image.checksum);
        return;
    }

    if (kCabinViewFlagRva + sizeof(std::uint32_t) > image.size) {
        log::warn("[camera-re] dormant probe disabled: target RVA outside image");
        return;
    }

    target_ = reinterpret_cast<std::uintptr_t>(image.base) + kCabinViewFlagRva;
    if (!target_is_in_data(image, target_)) {
        log::warn("[camera-re] dormant probe disabled: +0x{:X} is not inside .data",
                  kCabinViewFlagRva);
        target_ = 0;
        return;
    }

    trigger_path_ = make_trigger_path();
    if (trigger_path_.empty()) {
        log::warn("[camera-re] dormant probe disabled: could not derive trigger path");
        target_ = 0;
        return;
    }

    try {
        worker_ = std::jthread([this](std::stop_token stop) { worker(stop); });
    } catch (...) {
        target_ = 0;
        log::warn("[camera-re] dormant probe disabled: worker creation failed");
        return;
    }

    log::info(
        "[camera-re] dormant; no VEH/DR/thread suspension until fh6-radio-rework\\camera_re_arm.txt exists");
}

CameraReProbe::~CameraReProbe() {
    if (worker_.joinable()) {
        worker_.request_stop();
        try {
            worker_.join();
        } catch (...) {
        }
    }
}

bool CameraReProbe::consume_trigger() noexcept {
    if (trigger_path_.empty()) return false;

    const DWORD attrs = GetFileAttributesW(trigger_path_.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    // Consume first. If the later arm attempt fails, creating the marker again
    // explicitly starts another experiment instead of silently retrying.
    DeleteFileW(trigger_path_.c_str());
    return true;
}

bool CameraReProbe::begin_session() noexcept {
    if (!target_ || InterlockedCompareExchange(&enabled_, 0, 0) != 0) return false;

    if (IsDebuggerPresent()) {
        log::warn("[camera-re] trigger ignored: debugger attached");
        return false;
    }

    const auto initial = *reinterpret_cast<volatile const std::uint32_t*>(target_);
    if (initial > 1) {
        log::warn("[camera-re] trigger ignored: cabin flag is {}, expected 0/1", initial);
        return false;
    }

    for (auto& slot : events_) {
        InterlockedExchange(&slot.ready, 0);
    }
    InterlockedExchange(&next_event_, 0);
    InterlockedExchange(&last_value_, static_cast<LONG>(initial));
    next_flush_ = 0;
    armed_threads_.clear();

    if (InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_active_probe), this, nullptr) != nullptr) {
        log::warn("[camera-re] trigger ignored: another probe session is active");
        return false;
    }

    veh_handle_ = AddVectoredExceptionHandler(1, &CameraReProbe::vectored_handler);
    if (!veh_handle_) {
        InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_active_probe), nullptr, this);
        log::warn("[camera-re] AddVectoredExceptionHandler failed error={}", GetLastError());
        return false;
    }

    // Keep the handler live before the first thread receives DR0.
    InterlockedExchange(&enabled_, 1);

    const auto armed = arm_existing_threads_once();
    if (armed == 0) {
        end_session("no eligible thread could be armed");
        return false;
    }

    log::info(
        "[camera-re] ARMED ONCE target=+0x{:X} absolute=0x{:X} initial={} threads={} timeout=20s",
        kCabinViewFlagRva, target_, initial, armed);
    return true;
}

void CameraReProbe::end_session(const char* reason) noexcept {
    if (InterlockedCompareExchange(&enabled_, 0, 0) == 0 && !veh_handle_) return;

    // Keep enabled_ and VEH live while removing DR0 from every thread we changed.
    disarm_armed_threads();

    InterlockedExchange(&enabled_, 0);

    if (veh_handle_) {
        RemoveVectoredExceptionHandler(veh_handle_);
        veh_handle_ = nullptr;
    }

    InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_active_probe), nullptr, this);

    log::info("[camera-re] DISARMED: {} captured_transitions={}",
              reason ? reason : "session ended",
              InterlockedCompareExchange(&next_event_, 0, 0));
}

LONG CALLBACK CameraReProbe::vectored_handler(EXCEPTION_POINTERS* pointers) noexcept {
    auto* probe = static_cast<CameraReProbe*>(InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_active_probe), nullptr, nullptr));
    if (!probe) return EXCEPTION_CONTINUE_SEARCH;
    return probe->handle_exception(pointers);
}

LONG CameraReProbe::handle_exception(EXCEPTION_POINTERS* pointers) noexcept {
    if (InterlockedCompareExchange(&enabled_, 0, 0) == 0 || !pointers ||
        !pointers->ExceptionRecord || !pointers->ContextRecord ||
        pointers->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT& ctx = *pointers->ContextRecord;

    constexpr std::uint64_t kB0 = 1ull << 0;
    constexpr std::uint64_t kL0 = 1ull << 0;
    constexpr unsigned kControlShift = 16;
    const auto control =
        (static_cast<std::uint64_t>(ctx.Dr7) >> kControlShift) & 0xFu;

    if ((static_cast<std::uint64_t>(ctx.Dr6) & kB0) == 0 ||
        (static_cast<std::uint64_t>(ctx.Dr7) & kL0) == 0 ||
        control != kWrite4ControlNibble ||
        static_cast<std::uintptr_t>(ctx.Dr0) != target_) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto current = *reinterpret_cast<volatile const std::uint32_t*>(target_);
    const auto previous = static_cast<std::uint32_t>(
        InterlockedExchange(&last_value_, static_cast<LONG>(current)));

    if (current <= 1 && previous <= 1 && current != previous) {
        const LONG index = InterlockedIncrement(&next_event_) - 1;
        if (index >= 0 && static_cast<std::size_t>(index) < events_.size()) {
            auto& slot = events_[static_cast<std::size_t>(index)];
            auto& event = slot.event;

            event.thread_id = GetCurrentThreadId();
            event.previous_value = previous;
            event.current_value = current;
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

    // Consume only our DR0 single-step condition. We arm a thread only when it
    // has no pre-existing enabled hardware breakpoints.
    ctx.Dr6 &= ~kB0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool CameraReProbe::arm_thread(DWORD thread_id, ArmedThread& saved) noexcept {
    if (!target_ || thread_id == worker_thread_id_ || thread_id == GetCurrentThreadId()) {
        return false;
    }

    HANDLE thread =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                   FALSE, thread_id);
    if (!thread) return false;

    bool armed = false;
    {
        ScopedSuspend suspend{thread};
        if (suspend.ok()) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

            if (GetThreadContext(thread, &ctx)) {
                const auto original_dr7 = static_cast<std::uint64_t>(ctx.Dr7);

                // L0/G0 ... L3/G3 occupy DR7 bits 0..7. If any are enabled,
                // leave the thread completely untouched.
                if ((original_dr7 & 0xFFu) == 0) {
                    saved.thread_id = thread_id;
                    saved.original_dr0 = ctx.Dr0;
                    saved.original_dr6 = ctx.Dr6;
                    saved.original_dr7 = ctx.Dr7;

                    auto dr7 = original_dr7;
                    dr7 &= ~(std::uint64_t{3} << 0);       // clear L0/G0
                    dr7 &= ~(std::uint64_t{0xF} << 16);    // clear R/W0+LEN0
                    dr7 |= std::uint64_t{1} << 0;          // L0
                    dr7 |= kWrite4ControlNibble << 16;     // write, 4 bytes

                    ctx.Dr0 = target_;
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

std::size_t CameraReProbe::arm_existing_threads_once() noexcept {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    const DWORD process_id = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process_id ||
                entry.th32ThreadID == worker_thread_id_) {
                continue;
            }

            ArmedThread saved{};
            if (arm_thread(entry.th32ThreadID, saved)) {
                armed_threads_.push_back(saved);
            }
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return armed_threads_.size();
}

void CameraReProbe::disarm_armed_threads() noexcept {
    for (const auto& saved : armed_threads_) {
        if (saved.thread_id == 0 || saved.thread_id == GetCurrentThreadId()) continue;

        HANDLE thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                       FALSE, saved.thread_id);
        if (!thread) continue;

        {
            ScopedSuspend suspend{thread};
            if (suspend.ok()) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

                if (GetThreadContext(thread, &ctx)) {
                    const auto control =
                        (static_cast<std::uint64_t>(ctx.Dr7) >> 16) & 0xFu;

                    // Restore only if DR0 still looks exactly like our breakpoint.
                    if ((static_cast<std::uint64_t>(ctx.Dr7) & 1u) != 0 &&
                        control == kWrite4ControlNibble &&
                        static_cast<std::uintptr_t>(ctx.Dr0) == target_) {
                        ctx.Dr0 = saved.original_dr0;
                        ctx.Dr6 = saved.original_dr6;
                        ctx.Dr7 = saved.original_dr7;
                        SetThreadContext(thread, &ctx);
                    }
                }
            }
        }

        CloseHandle(thread);
    }

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

        log::info(
            "[camera-re] HIT #{} tid={} flag {}->{} trap=+0x{:X} function=+0x{:X} dr6=0x{:X} dr7=0x{:X}",
            next_flush_ + 1, event.thread_id,
            event.previous_value, event.current_value,
            trap_rva, function_rva, event.dr6, event.dr7);

        log::info(
            "[camera-re] regs rax={:016X} rbx={:016X} rcx={:016X} rdx={:016X} rsi={:016X} rdi={:016X} rbp={:016X} rsp={:016X}",
            event.regs[0], event.regs[1], event.regs[2], event.regs[3],
            event.regs[4], event.regs[5], event.regs[6], event.regs[7]);

        log::info(
            "[camera-re] regs r8={:016X} r9={:016X} r10={:016X} r11={:016X} r12={:016X} r13={:016X} r14={:016X} r15={:016X} rflags={:X}",
            event.regs[8], event.regs[9], event.regs[10], event.regs[11],
            event.regs[12], event.regs[13], event.regs[14], event.regs[15],
            event.rflags);

        if (!bytes.empty()) {
            log::info("[camera-re] code +0x{:X}: {}", code_start_rva, bytes);
        }

        InterlockedExchange(&slot.ready, 0);
        ++next_flush_;
    }
}

void CameraReProbe::worker(std::stop_token stop) noexcept {
    worker_thread_id_ = GetCurrentThreadId();
    auto deadline = std::chrono::steady_clock::time_point::max();

    try {
        while (!stop.stop_requested()) {
            const bool enabled = InterlockedCompareExchange(&enabled_, 0, 0) != 0;

            if (!enabled) {
                if (consume_trigger() && begin_session()) {
                    deadline = std::chrono::steady_clock::now() + kSessionTimeout;
                }
                std::this_thread::sleep_for(100ms);
                continue;
            }

            flush_events();

            const LONG captured = InterlockedCompareExchange(&next_event_, 0, 0);
            if (captured >= 2) {
                end_session("captured two value-changing writes");
                deadline = std::chrono::steady_clock::time_point::max();
                continue;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                end_session("20 second timeout");
                deadline = std::chrono::steady_clock::time_point::max();
                continue;
            }

            std::this_thread::sleep_for(50ms);
        }
    } catch (...) {
        log::error("[camera-re] worker failed");
    }

    if (InterlockedCompareExchange(&enabled_, 0, 0) != 0 || veh_handle_) {
        end_session("probe shutdown");
    }

    try {
        flush_events();
    } catch (...) {
    }
}

} // namespace fh6r
