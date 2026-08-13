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

// Dump the preceding bytes of a call site and try to identify the RCX source
// with a few common patterns only (no general data-flow engine). Reads .text,
// never executes anything.
void dump_xref_context(std::byte* call_site) {
    // Dump 48 bytes before the call for manual inspection.
    std::byte* start = call_site - 48;
    char hex[160]{};
    int h = 0;
    for (int i = 0; i < 48 && h < (int)sizeof(hex) - 4; ++i)
        h += std::snprintf(hex + h, sizeof(hex) - h, "%02X ", static_cast<unsigned char>(start[i]));
    log::info("[studio-system]   ctx: {}", hex);

    // Scan backward for the nearest instruction that writes RCX, using fixed
    // byte patterns. Only a handful of shapes are recognized; anything else is
    // reported as "no recognized rcx writer".
    const std::byte* p = call_site;
    for (int back = 1; back <= 48; ++back) {
        const std::byte* q = call_site - back;
        const unsigned char* b = reinterpret_cast<const unsigned char*>(q);

        // lea rcx, [rip+disp32] : 48 8D 0D xx xx xx xx
        if (back + 7 <= 48 && b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x0D) {
            std::int32_t disp = 0; std::memcpy(&disp, q + 3, 4);
            std::byte* target = call_site - back + 7 + disp;
            log::info("[studio-system]   RCX = &[rip+{:d}] -> abs 0x{:X} (lea)", disp,
                      reinterpret_cast<std::uintptr_t>(target));
            return;
        }
        // mov rcx, [rip+disp32] : 48 8B 0D xx xx xx xx
        if (back + 7 <= 48 && b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x0D) {
            std::int32_t disp = 0; std::memcpy(&disp, q + 3, 4);
            std::byte* target = call_site - back + 7 + disp;
            log::info("[studio-system]   RCX = [rip+{:d}] -> abs 0x{:X} (mov)", disp,
                      reinterpret_cast<std::uintptr_t>(target));
            return;
        }
        // mov rcx, [reg+disp8] : 48 8B 4? xx   (rm = ? in 0..7)
        if (back + 4 <= 48 && b[0] == 0x48 && b[1] == 0x8B && (b[2] & 0xC0) == 0x40) {
            const int rm = b[2] & 7;
            const int disp8 = static_cast<std::int8_t>(b[3]);
            log::info("[studio-system]   RCX = [{}+{:d}]", reg_name(rm), disp8);
            return;
        }
        // mov rcx, [reg+disp32] : 48 8B 8? xx xx xx xx
        if (back + 7 <= 48 && b[0] == 0x48 && b[1] == 0x8B && (b[2] & 0xC0) == 0x80) {
            const int rm = b[2] & 7;
            std::int32_t disp = 0; std::memcpy(&disp, q + 3, 4);
            log::info("[studio-system]   RCX = [{}+0x{:X}]", reg_name(rm), disp);
            return;
        }
        // mov rcx, reg : 48 8B C?  (rm = reg, mod=11)
        if (back + 3 <= 48 && b[0] == 0x48 && b[1] == 0x8B && (b[2] & 0xC0) == 0xC0) {
            const int rm = b[2] & 7;
            log::info("[studio-system]   RCX = {}", reg_name(rm));
            return;
        }
    }
    log::info("[studio-system]   (no recognized RCX writer in 48-byte window)");
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
        for (std::byte* p = img.text; p + 5 <= img.text_end; ++p) {
            if (p[0] != std::byte{0xE8}) continue;
            std::int32_t disp = 0;
            std::memcpy(&disp, p + 1, 4);
            std::byte* target = p + 5 + disp;
            if (target != fn) continue;
            ++xrefs;
            log::info("[studio-system] {} (0x{:X}) xref at RVA 0x{:X}",
                      a, reinterpret_cast<std::uintptr_t>(fn),
                      static_cast<std::uintptr_t>(p - img.base));
            dump_xref_context(p);
        }
        log::info("[studio-system] {} = 0x{:X} -> {} xref(s)", a,
                  reinterpret_cast<std::uintptr_t>(fn), xrefs);
    }
    log::info("[studio-system] ===== done =====");
}

} // namespace fh6r::fmod
