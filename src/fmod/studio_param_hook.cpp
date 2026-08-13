#include "fh6r/fmod/studio_param_hook.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace fh6r::fmod {
namespace {

// FMOD Studio setter ABIs (x64): RCX=this, RDX=name/id, XMM2=value, R9=ignoreSeekSpeed.
// FMOD_STUDIO_PARAMETER_ID is { uint32 data1; uint32 data2; } -> passed as one u64 in RDX.
using SetParamByNameFn = std::uint32_t (*)(void*, const char*, float, bool);
using SetParamByIDFn   = std::uint32_t (*)(void*, std::uint64_t, float, bool);

std::atomic<std::uint64_t> g_by_name_total{0};
std::atomic<std::uint64_t> g_by_id_total{0};

// Trampoline storage written BEFORE the target is patched, so there is no
// window where the hook runs with a null original pointer.
void* g_tramp_name = nullptr;
void* g_tramp_id   = nullptr;

// Conservative x86-64 instruction-length decoder for *prologue relocation*.
// Walks complete instructions starting at `code`, accumulating until >= min_len
// bytes are covered, and returns the byte count (guaranteed instruction
// boundary). Returns 0 (bail) on anything not safely relocatable:
//   - mod==00 addressing (covers RIP-relative and absolute [disp32])
//   - branches/calls/unrecognised opcodes
// Only the register/stack forms seen in MSVC function prologues are accepted.
std::size_t safe_prologue_len(const std::uint8_t* code, std::size_t avail,
                              std::size_t min_len) noexcept {
    std::size_t pos = 0;
    while (pos < min_len) {
        if (pos >= avail) return 0;
        std::uint8_t b = code[pos];
        std::size_t insn = 0;

        // REX prefix (0x40..0x4F)
        if (b >= 0x40 && b <= 0x4F) {
            ++insn;
            if (pos + insn >= avail) return 0;
            b = code[pos + insn];
        }
        ++insn; // opcode byte

        switch (b) {
            // push/pop r64 / r8-r15 (no ModRM)
            case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
            case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                break;

            // MOV / LEA / ADD / SUB / XOR / MOVSXD (ModRM)
            case 0x89: case 0x8B: case 0x8D: case 0x8A: case 0x88:
            case 0x03: case 0x2B: case 0x33: case 0x63: {
                if (pos + insn >= avail) return 0;
                const std::uint8_t modrm = code[pos + insn];
                ++insn;
                const std::uint8_t mod = (modrm >> 6) & 3;
                const std::uint8_t rm  = modrm & 7;
                if (mod == 0) return 0; // rip-relative / absolute: not relocatable
                if (mod != 3 && rm == 4) ++insn;      // SIB
                if (mod == 1) ++insn;                 // disp8
                else if (mod == 2) insn += 4;         // disp32
                break;
            }

            // Group 1: r/m, imm (ADD/SUB/CMP/AND/... imm8/imm32)
            case 0x81: case 0x83: {
                if (pos + insn >= avail) return 0;
                const std::uint8_t modrm = code[pos + insn];
                ++insn;
                const std::uint8_t mod = (modrm >> 6) & 3;
                const std::uint8_t rm  = modrm & 7;
                if (mod == 0) return 0;
                if (mod != 3 && rm == 4) ++insn;      // SIB
                if (mod == 1) ++insn;
                else if (mod == 2) insn += 4;
                insn += (b == 0x81) ? 4 : 1;          // imm32 / imm8
                break;
            }

            default:
                return 0; // unknown -> refuse to relocate
        }

        pos += insn;
        if (pos > avail) return 0;
    }
    return pos;
}

// Write an absolute 12-byte jump "mov rax, imm64; jmp rax" at `dst` -> `target`.
void write_abs_jump(std::uint8_t* dst, const void* target) noexcept {
    dst[0] = 0x48; dst[1] = 0xB8;
    std::memcpy(dst + 2, &target, 8);
    dst[10] = 0xFF; dst[11] = 0xE0;
}

// Install detour on `target` -> `hook`. On success writes the trampoline into
// *out_tramp BEFORE patching the target (so the hook never observes a null
// original) and returns true; returns false on failure (target left untouched).
bool install_detour(std::byte* target, void* hook, const char* what, void** out_tramp) noexcept {
    if (!target || !hook) return false;
    std::uint8_t prologue[32]{};
    if (!is_readable(target, sizeof(prologue))) {
        log::warn("[setparam] {}: target 0x{:X} not readable, skip",
                  what, reinterpret_cast<std::uintptr_t>(target));
        return false;
    }
    std::memcpy(prologue, target, sizeof(prologue));

    const std::size_t n = safe_prologue_len(prologue, sizeof(prologue), 12);
    if (n == 0) {
        // Dump the first bytes so the next iteration can hand-write a pattern.
        char hex[64]{};
        std::size_t h = 0;
        for (std::size_t i = 0; i < 16 && h < sizeof(hex) - 4; ++i)
            h += std::snprintf(hex + h, sizeof(hex) - h, "%02X ", prologue[i]);
        log::warn("[setparam] {}: prologue not relocatable, skip (bytes: {})", what, hex);
        return false;
    }

    // Trampoline: saved prologue + absolute jump back to target+n.
    void* tramp = VirtualAlloc(nullptr, n + 12, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        log::warn("[setparam] {}: VirtualAlloc trampoline failed, skip", what);
        return false;
    }
    std::uint8_t* t = static_cast<std::uint8_t*>(tramp);
    std::memcpy(t, prologue, n);
    write_abs_jump(t + n, target + n);
    FlushInstructionCache(GetCurrentProcess(), tramp, n + 12);

    // Publish the trampoline BEFORE patching the live function.
    if (out_tramp) *out_tramp = tramp;

    // Overwrite the function prologue with the absolute jump to the hook.
    DWORD old = 0;
    if (!VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE, &old)) {
        log::warn("[setparam] {}: VirtualProtect failed, skip", what);
        VirtualFree(tramp, 0, MEM_RELEASE);
        if (out_tramp) *out_tramp = nullptr;
        return false;
    }
    std::uint8_t jmp[12];
    write_abs_jump(jmp, hook);
    std::memcpy(target, jmp, 12);
    VirtualProtect(target, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 12);

    log::info("[setparam] {} hooked: fn=0x{:X} -> hook=0x{:X} (relocated {} bytes)",
              what, reinterpret_cast<std::uintptr_t>(target),
              reinterpret_cast<std::uintptr_t>(hook), n);
    return true;
}

// Bounded, readability-checked read of a C string into a std::string_view.
std::string_view safe_cstr(const char* s, std::size_t max_len = 128) noexcept {
    if (!s) return {};
    std::size_t n = 0;
    while (n < max_len && is_readable(s + n, 1) && s[n] != '\0') ++n;
    return {s, n};
}

bool interesting_name(std::string_view name) noexcept {
    return name.find("ockpit") != std::string_view::npos  || // Cockpit
           name.find("amera")  != std::string_view::npos  || // Camera
           name.find("nterior")!= std::string_view::npos  || // Interior
           name.find("xterior")!= std::string_view::npos  || // Exterior
           name.find("istener")!= std::string_view::npos  || // Listener
           name.find("oof")     != std::string_view::npos  || // Roof
           name.find("oppler")  != std::string_view::npos;    // Doppler
}

// ---- hooks ----
std::uint32_t hook_set_param_by_name(void* sys, const char* name, float value,
                                     bool ignore_seek) noexcept {
    const std::uint64_t total = g_by_name_total.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::string_view sv = safe_cstr(name);
    if (interesting_name(sv)) {
        log::info("[setparam] NAME '{}' = {:.4f}", sv, value);
    } else if ((total & 0x3FF) == 0) { // sample every 1024 non-interesting calls
        log::info("[setparam] (sampled {}) NAME '{}' = {:.4f}", total, sv, value);
    }
    return reinterpret_cast<SetParamByNameFn>(g_tramp_name)(sys, name, value, ignore_seek);
}

std::uint32_t hook_set_param_by_id(void* sys, std::uint64_t id, float value,
                                   bool ignore_seek) noexcept {
    const std::uint64_t total = g_by_id_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((total & 0xFFF) == 0) { // sample every 4096 (id has no name here)
        log::info("[setparam] BYID id={:08X}:{:08X} value={:.4f} [total {}]",
                  static_cast<unsigned>(id & 0xFFFFFFFFu),
                  static_cast<unsigned>(id >> 32), value, total);
    }
    return reinterpret_cast<SetParamByIDFn>(g_tramp_id)(sys, id, value, ignore_seek);
}

} // namespace

namespace {
// Resolve a Studio method by anchor, filtering the shared dispatch function and
// LOGGING every candidate (address + prologue). Returns the "real" candidates.
// If it returns exactly one, that is the hook target; otherwise the caller logs
// the ambiguity and skips (the dumped prologues let the next iteration pick).
constexpr std::uint8_t kDispatchPrologue[8] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57};

std::vector<std::byte*> resolve_and_log(const PEImage& img, std::string_view anchor) {
    const auto cands = scout_anchor(img, anchor);
    std::vector<std::byte*> real;
    log::info("[setparam] '{}' -> {} raw candidate(s):", anchor, cands.size());
    for (auto* fn : cands) {
        std::uint8_t pre[8]{};
        const bool rd = is_readable(fn, sizeof(pre));
        if (rd) std::memcpy(pre, fn, sizeof(pre));
        const bool dispatch = rd && std::memcmp(pre, kDispatchPrologue, sizeof(pre)) == 0;
        char hex[64]{};
        std::size_t h = 0;
        for (std::size_t i = 0; i < 16 && h < sizeof(hex) - 4; ++i) {
            std::uint8_t b = 0;
            if (!is_readable(fn + i, 1)) break;
            std::memcpy(&b, fn + i, 1);
            h += std::snprintf(hex + h, sizeof(hex) - h, "%02X ", b);
        }
        log::info("[setparam]   0x{:X} prologue=[{}]{}",
                  reinterpret_cast<std::uintptr_t>(fn), hex, dispatch ? "  [DISPATCH]" : "");
        if (!dispatch) real.push_back(fn);
    }
    return real;
}

// Hook the first (and expected only) candidate; logs whether it was ambiguous.
void hook_if_unique(std::vector<std::byte*>& cands, const char* what,
                    void* hook_fn, void** out_tramp) {
    if (cands.empty()) {
        log::warn("[setparam] {}: no real candidate (all were dispatch)", what);
        return;
    }
    if (cands.size() > 1) {
        log::warn("[setparam] {}: {} real candidates -> NOT hooking (ambiguous)", what, cands.size());
        return;
    }
    install_detour(cands[0], hook_fn, what, out_tramp);
}
} // namespace

void install_studio_param_hooks(const PEImage& img) noexcept {
    if (!img.valid()) { log::warn("[setparam] image invalid, skip"); return; }
    log::info("[setparam] ===== studio parameter hook install start =====");

    auto by_name = resolve_and_log(img, "System::setParameterByName");
    auto by_id   = resolve_and_log(img, "System::setParameterByID");

    hook_if_unique(by_name, "setParameterByName",
                   reinterpret_cast<void*>(&hook_set_param_by_name), &g_tramp_name);
    hook_if_unique(by_id, "setParameterByID",
                   reinterpret_cast<void*>(&hook_set_param_by_id), &g_tramp_id);

    log::info("[setparam] ===== install done (byName={} byId={}) =====",
              g_tramp_name ? "hooked" : "skipped",
              g_tramp_id ? "hooked" : "skipped");
}

} // namespace fh6r::fmod

