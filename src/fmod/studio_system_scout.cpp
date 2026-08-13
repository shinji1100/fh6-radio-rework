#include "fh6r/fmod/studio_system_scout.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fh6r::fmod {
namespace {

const char* reg_name(int r) {
    static const char* names[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"};
    return (r >= 0 && r < 8) ? names[r] : "?";
}

void dump_hex(std::byte* p, int n, const char* tag) {
    char hex[192]{};
    int h = 0;
    for (int i = 0; i < n && h < (int)sizeof(hex) - 4; ++i)
        h += std::snprintf(hex + h, sizeof(hex) - h, "%02X ", static_cast<unsigned char>(p[i]));
    log::info("[studio-system]   {}: {}", tag, hex);
}

// Identify the RCX source at a call site with a few fixed byte patterns only.
// Recognizes the three legitimate shapes:
//   A. lea rcx,[rip+disp]  -> global slot (out-param = &gStudioSystem)
//   B. lea/mov rcx,[reg+disp] -> object field slot
//   C. lea rcx,[rsp/rbp+disp] -> stack temp (value is copied out after the call)
// Returns true when a writer was recognized, false otherwise.
bool trace_rcx(std::byte* call_site) {
    for (int back = 1; back <= 48; ++back) {
        const std::byte* q = call_site - back;
        const unsigned char* b = reinterpret_cast<const unsigned char*>(q);

        // lea rcx, [rip+disp32] : 48 8D 0D xx xx xx xx  (A: global slot)
        if (back + 7 <= 48 && b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x0D) {
            std::int32_t disp = 0; std::memcpy(&disp, q + 3, 4);
            std::byte* target = call_site - back + 7 + disp;
            log::info("[studio-system]   RCX = &[rip+{:d}] -> abs 0x{:X}  [A: global slot]",
                      disp, reinterpret_cast<std::uintptr_t>(target));
            return true;
        }
        // mov rcx, [rip+disp32] : 48 8B 0D xx xx xx xx
        if (back + 7 <= 48 && b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x0D) {
            std::int32_t disp = 0; std::memcpy(&disp, q + 3, 4);
            std::byte* target = call_site - back + 7 + disp;
            log::info("[studio-system]   RCX = [rip+{:d}] -> abs 0x{:X}  [global value]",
                      disp, reinterpret_cast<std::uintptr_t>(target));
            return true;
        }
        // lea rcx, [rsp+disp8] : 48 8D 4C 24 xx  (C: stack temp)
        if (back + 5 <= 48 && b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x4C && b[3] == 0x24) {
            const int disp8 = static_cast<std::int8_t>(b[4]);
            log::info("[studio-system]   RCX = &[rsp+{:d}]  [C: stack temp]", disp8);
            dump_hex(call_site + 5, 16, "post-call");
            return true;
        }
        // lea rcx, [rbp+disp8] : 48 8D 4D xx  (C: stack temp, frame-relative)
        if (back + 4 <= 48 && b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x4D) {
            const int disp8 = static_cast<std::int8_t>(b[3]);
            log::info("[studio-system]   RCX = &[rbp+{:d}]  [C: stack temp]", disp8);
            dump_hex(call_site + 5, 16, "post-call");
            return true;
        }
        // lea rcx, [rsp+disp32] : 48 8D 8C 24 xx xx xx xx
        if (back + 8 <= 48 && b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x8C && b[3] == 0x24) {
            std::int32_t disp = 0; std::memcpy(&disp, q + 4, 4);
            log::info("[studio-system]   RCX = &[rsp+0x{:X}]  [C: stack temp]", disp);
            dump_hex(call_site + 5, 16, "post-call");
            return true;
        }
        // mov rcx, [reg+disp8] : 48 8B 4? xx  (B: object field)
        if (back + 4 <= 48 && b[0] == 0x48 && b[1] == 0x8B && (b[2] & 0xC0) == 0x40) {
            const int rm = b[2] & 7;
            const int disp8 = static_cast<std::int8_t>(b[3]);
            log::info("[studio-system]   RCX = [{}+{:d}]  [B: object field]", reg_name(rm), disp8);
            return true;
        }
        // mov rcx, [reg+disp32] : 48 8B 8? xx xx xx xx
        if (back + 7 <= 48 && b[0] == 0x48 && b[1] == 0x8B && (b[2] & 0xC0) == 0x80) {
            const int rm = b[2] & 7;
            std::int32_t disp = 0; std::memcpy(&disp, q + 3, 4);
            log::info("[studio-system]   RCX = [{}+0x{:X}]  [B: object field]", reg_name(rm), disp);
            return true;
        }
        // mov rcx, reg : 48 8B C?
        if (back + 3 <= 48 && b[0] == 0x48 && b[1] == 0x8B && (b[2] & 0xC0) == 0xC0) {
            const int rm = b[2] & 7;
            log::info("[studio-system]   RCX = {}  [register forward]", reg_name(rm));
            return true;
        }
    }
    return false;
}

} // namespace

void scout_studio_system(const PEImage& img) noexcept {
    if (!img.valid()) { log::warn("[studio-system] image invalid, skipping"); return; }
    log::info("[studio-system] ===== resolve factory/lifecycle + xref scan =====");

    const char* anchors[] = {"System::create", "System::getCoreSystem",
                             "System::initialize", "System::update"};
    for (const char* a : anchors) {
        std::byte* fn = resolve_studio_anchor(img, a);
        if (!fn) {
            log::info("[studio-system] {} = NULL (MISSING or AMBIGUOUS)", a);
            continue;
        }
        int xrefs = 0;

        // Pass 1: E8 rel32 direct calls.
        for (std::byte* p = img.text; p + 5 <= img.text_end; ++p) {
            if (p[0] != std::byte{0xE8}) continue;
            std::int32_t disp = 0; std::memcpy(&disp, p + 1, 4);
            if (p + 5 + disp != fn) continue;
            ++xrefs;
            log::info("[studio-system] {} (0x{:X}) E8 xref at RVA 0x{:X}",
                      a, reinterpret_cast<std::uintptr_t>(fn),
                      static_cast<std::uintptr_t>(p - img.base));
            dump_hex(p - 48, 48, "ctx");
            if (!trace_rcx(p)) log::info("[studio-system]   (no recognized RCX writer)");
        }

        // Pass 2: FF 15 [rip+disp32] RIP-indirect calls (via function pointer).
        for (std::byte* p = img.text; p + 6 <= img.text_end; ++p) {
            if (p[0] != std::byte{0xFF} || p[1] != std::byte{0x15}) continue;
            std::int32_t disp = 0; std::memcpy(&disp, p + 2, 4);
            std::byte* slot = p + 6 + disp;
            std::byte* target = nullptr;
            if (!safe_read(slot, target) || target != fn) continue;
            ++xrefs;
            log::info("[studio-system] {} (0x{:X}) FF15 xref at RVA 0x{:X} (slot 0x{:X})",
                      a, reinterpret_cast<std::uintptr_t>(fn),
                      static_cast<std::uintptr_t>(p - img.base),
                      reinterpret_cast<std::uintptr_t>(slot));
            dump_hex(p - 48, 48, "ctx");
            if (!trace_rcx(p)) log::info("[studio-system]   (no recognized RCX writer)");
        }

        log::info("[studio-system] {} = 0x{:X} -> {} xref(s)", a,
                  reinterpret_cast<std::uintptr_t>(fn), xrefs);
    }
    log::info("[studio-system] ===== done =====");
}

} // namespace fh6r::fmod
