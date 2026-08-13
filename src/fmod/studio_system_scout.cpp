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
    static const char* names[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                  "r8","r9","r10","r11","r12","r13","r14","r15"};
    return (r >= 0 && r < 16) ? names[r] : "?";
}

constexpr int R_RCX = 1;
constexpr int R_RSP = 4;
constexpr int R_RBP = 5;

// A decoded mov/lea instruction (the only shapes we care about for tracing).
struct Insn {
    int op = 0;          // 0x8B = mov load, 0x89 = mov store, 0x8D = lea
    int dst_reg = -1;    // register written (mov load / lea / reg-reg store)
    bool src_is_mem = false;
    bool rip_rel = false;
    int base = -1;       // memory base register
    int disp = 0;
    int src_reg = -1;    // register source (reg-reg move)
    int len = 0;
};

// Decode a mov/lea instruction at q (with REX.W/R/X/B). Returns length or 0 if
// not a recognized shape.
int decode(const std::byte* q, const std::byte* end, Insn& out) {
    if (end - q < 2) return 0;
    int i = 0;
    int rex = 0;
    if ((std::to_integer<std::uint8_t>(q[0]) & 0xF0) == 0x40) { rex = std::to_integer<std::uint8_t>(q[0]); i = 1; }
    const int op = std::to_integer<std::uint8_t>(q[i]);
    if (op != 0x8B && op != 0x89 && op != 0x8D) return 0;
    if (end - q < i + 2) return 0;
    const int modrm = std::to_integer<std::uint8_t>(q[i + 1]);
    const int mod = modrm >> 6;
    const int reg = (modrm >> 3) & 7;
    const int rm = modrm & 7;
    const bool rex_w = rex & 8, rex_r = rex & 4, rex_x = rex & 2, rex_b = rex & 1;
    const int r = reg | (rex_r ? 8 : 0);
    const int m = rm | (rex_b ? 8 : 0);
    int len = i + 2;

    out = Insn{};
    out.op = op;

    if (mod == 3) {  // register operand
        out.dst_reg = (op == 0x89) ? m : r;
        out.src_reg = (op == 0x89) ? r : m;
        out.len = len;
        return len;
    }

    // memory operand
    int base = -1;
    int disp = 0;
    bool rip_rel = false;
    if (mod == 0 && rm == 5) {  // [rip+disp32]
        if (end - q < len + 4) return 0;
        disp = std::to_integer<std::int32_t>(*reinterpret_cast<const std::int32_t*>(q + len));
        len += 4;
        rip_rel = true;
    } else if (rm == 4) {  // SIB
        if (end - q < len + 1) return 0;
        const int sib = std::to_integer<std::uint8_t>(q[len]); len += 1;
        const int s_base = sib & 7;
        const int s_index = (sib >> 3) & 7;
        if (s_index != 4) return 0;  // indexed addressing not supported (rare here)
        base = s_base | (rex_b ? 8 : 0);
        if (mod == 0 && s_base == 5) {  // disp32 only
            if (end - q < len + 4) return 0;
            disp = std::to_integer<std::int32_t>(*reinterpret_cast<const std::int32_t*>(q + len));
            len += 4;
            base = -1;
            rip_rel = true;
        } else if (mod == 1) {
            if (end - q < len + 1) return 0;
            disp = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(q[len])); len += 1;
        } else if (mod == 2) {
            if (end - q < len + 4) return 0;
            disp = std::to_integer<std::int32_t>(*reinterpret_cast<const std::int32_t*>(q + len));
            len += 4;
        }
    } else {
        base = m;
        if (mod == 1) {
            if (end - q < len + 1) return 0;
            disp = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(q[len])); len += 1;
        } else if (mod == 2) {
            if (end - q < len + 4) return 0;
            disp = std::to_integer<std::int32_t>(*reinterpret_cast<const std::int32_t*>(q + len));
            len += 4;
        }
    }

    out.src_is_mem = true;
    out.rip_rel = rip_rel;
    out.base = base;
    out.disp = disp;
    out.dst_reg = (op == 0x89) ? -1 : r;   // mov store has no register dst
    out.src_reg = (op == 0x89) ? r : -1;   // mov store source is the reg field
    out.len = len;
    return len;
}

void dump_hex(std::byte* p, int n, const char* tag) {
    char hex[192]{};
    int h = 0;
    for (int i = 0; i < n && h < (int)sizeof(hex) - 4; ++i)
        h += std::snprintf(hex + h, sizeof(hex) - h, "%02X ", static_cast<unsigned char>(p[i]));
    log::info("[studio-system]   {}: {}", tag, hex);
}

// Trace the RCX source at a call site. Walk backward up to 4 layers of
// register data-flow to find the stable storage (global slot / object field /
// stack temp). Read-only.
void trace_rcx(std::byte* call_site) {
    int reg = R_RCX;
    std::byte* pos = call_site;
    bool reported = false;

    for (int layer = 0; layer < 4; ++layer) {
        bool found = false;
        for (int back = 1; back <= 48 && !found; ++back) {
            const std::byte* q = pos - back;
            if (q < call_site - 48) break;
            Insn insn;
            const int len = decode(q, pos, insn);
            if (len == 0) continue;
            if (insn.dst_reg != reg) continue;

            found = true;
            if (insn.rip_rel) {
                std::byte* target = const_cast<std::byte*>(q) + insn.len + insn.disp;
                log::info("[studio-system]   RCX-source: {} = &[rip+{:d}] -> abs 0x{:X}  [global slot]",
                          reg_name(reg), insn.disp, reinterpret_cast<std::uintptr_t>(target));
                reported = true;
            } else if (insn.src_is_mem && insn.base >= 0) {
                log::info("[studio-system]   RCX-source: {} = [{}+0x{:X}]  [object field]",
                          reg_name(reg), reg_name(insn.base), insn.disp);
                if (insn.base == R_RSP || insn.base == R_RBP) {
                    log::info("[studio-system]   (base is stack; value copied out after call)");
                    dump_hex(call_site + 5, 16, "post-call");
                    reported = true;
                } else {
                    reg = insn.base;   // keep tracing the base register
                    pos = q;
                }
            } else if (insn.src_reg >= 0) {
                log::info("[studio-system]   RCX-source: {} = {}", reg_name(reg), reg_name(insn.src_reg));
                reg = insn.src_reg;
                pos = q;
            } else {
                log::info("[studio-system]   RCX-source: {} (unrecognized writer)", reg_name(reg));
                reported = true;
            }
        }
        if (!found || reported) break;
    }
    if (!reported) {
        log::info("[studio-system]   (trace stopped: no writer found or depth limit)");
    }
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
            trace_rcx(p);
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
            trace_rcx(p);
        }

        log::info("[studio-system] {} = 0x{:X} -> {} xref(s)", a,
                  reinterpret_cast<std::uintptr_t>(fn), xrefs);
    }
    log::info("[studio-system] ===== done =====");
}

} // namespace fh6r::fmod
