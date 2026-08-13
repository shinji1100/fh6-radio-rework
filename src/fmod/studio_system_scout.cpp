#include "fh6r/fmod/studio_system_scout.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace fh6r::fmod {
namespace {

using namespace std::chrono_literals;

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

// Read a disp32 at q as a signed 32-bit value (memcpy avoids unaligned reads).
inline std::int32_t read_disp32(const std::byte* q) noexcept {
    std::int32_t v = 0;
    std::memcpy(&v, q, 4);
    return v;
}

// Decode a mov/lea instruction at q (with REX.W/R/X/B, r8-r15 covered). Returns
// length or 0 if not a recognized shape.
//
// NOTE on the special RIP-relative encoding: in 64-bit mode, ModRM mod=00 &&
// raw r/m=101 is ALWAYS [rip+disp32], regardless of REX.B. r13/rbp as a base
// register cannot use the mod=00 no-displacement form, so this is unambiguous.
// The special addressing encoding therefore takes precedence over the r/m
// register extension (which only applies to the *register* operand forms).
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
    (void)rex_w; (void)rex_x;
    const int r = reg | (rex_r ? 8 : 0);
    const int m = rm | (rex_b ? 8 : 0);
    int len = i + 2;

    out = Insn{};
    out.op = op;

    if (mod == 3) {  // register operand (r8-r15 handled by r/m extension)
        out.dst_reg = (op == 0x89) ? m : r;
        out.src_reg = (op == 0x89) ? r : m;
        out.len = len;
        return len;
    }

    // memory operand
    int base = -1;
    int disp = 0;
    bool rip_rel = false;
    if (mod == 0 && rm == 5) {  // [rip+disp32] — special encoding, REX.B does NOT apply
        if (end - q < len + 4) return 0;
        disp = read_disp32(q + len);
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
            disp = read_disp32(q + len);
            len += 4;
            base = -1;
            rip_rel = true;
        } else if (mod == 1) {
            if (end - q < len + 1) return 0;
            disp = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(q[len])); len += 1;
        } else if (mod == 2) {
            if (end - q < len + 4) return 0;
            disp = read_disp32(q + len);
            len += 4;
        }
    } else {
        base = m;
        if (mod == 1) {
            if (end - q < len + 1) return 0;
            disp = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(q[len])); len += 1;
        } else if (mod == 2) {
            if (end - q < len + 4) return 0;
            disp = read_disp32(q + len);
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

// Result of tracing the RCX source at a call site. The final target address is
//   target = base_value + disp
// where base_value is *(global) when global_load is true, or global itself when
// global_load is false (lea = address, mov = load).
struct RcxTrace {
    bool resolved = false;
    bool global_load = false;   // true: base = *(global); false: base = global (address)
    bool deref_boundary = false; // hit mov reg,[base+disp] — not algebraically flattenable
    std::byte* global = nullptr;
    int disp = 0;
};

// Walk backward from the call site through the register data-flow to find the
// stable storage of RCX. Handles lea [base+disp] (accumulates disp), reg-reg
// moves, and a final RIP-relative global source (mov = load / lea = address).
// Read-only; the decoded instruction bytes come from the live (decrypted) image.
RcxTrace trace_rcx_addr(std::byte* call_site) noexcept {
    RcxTrace out;
    int reg = R_RCX;
    const std::byte* pos = call_site;

    for (int layer = 0; layer < 8; ++layer) {
        bool advanced = false;
        for (int back = 1; back <= 64 && !advanced; ++back) {
            const std::byte* q = pos - back;
            if (q < call_site - 256) break;  // hard backward cap
            Insn insn;
            const int len = decode(q, pos, insn);
            if (len == 0) continue;
            if (insn.dst_reg != reg) continue;

            advanced = true;
            if (insn.rip_rel) {
                out.resolved = true;
                out.global = const_cast<std::byte*>(q) + insn.len + insn.disp;
                out.global_load = (insn.op == 0x8B);  // mov = load value; lea = address
                log::info("[studio-system]   base {} = {}{} -> global 0x{:X}",
                          reg_name(reg), insn.op == 0x8B ? "*" : "",
                          reg_name(reg), reinterpret_cast<std::uintptr_t>(out.global));
                return out;
            } else if (insn.op == 0x8D && insn.src_is_mem && insn.base >= 0) {
                log::info("[studio-system]   {} = {}+0x{:X}", reg_name(reg), reg_name(insn.base), insn.disp);
                out.disp += insn.disp;
                reg = insn.base;
                pos = q;
            } else if (insn.src_reg >= 0) {
                log::info("[studio-system]   {} = {}", reg_name(reg), reg_name(insn.src_reg));
                reg = insn.src_reg;
                pos = q;
            } else if (insn.src_is_mem && insn.base >= 0) {
                // mov reg,[base+disp] is a memory LOAD: reg = *(base+disp). This is a
                // dereference boundary — the displacement cannot be accumulated like
                // a lea (*(base+disp) != base+disp). Stop flattening here instead of
                // producing a plausible-but-wrong slot address.
                log::info("[studio-system]   {} = *[{}+0x{:X}] [DEREF boundary, stop]",
                          reg_name(reg), reg_name(insn.base), insn.disp);
                out.deref_boundary = true;
                return out;
            } else {
                log::info("[studio-system]   {} = (unrecognized writer)", reg_name(reg));
                return out;
            }
        }
        if (!advanced) {
            log::info("[studio-system]   (no writer for {} within window; likely a function arg/this)", reg_name(reg));
            break;
        }
    }
    return out;
}

// Trace one create() call site and read the Studio System handle from the slot
// it derives. Returns the handle, or nullptr (not resolved / not yet populated).
std::byte* read_handle_from_site(std::byte* site) noexcept {
    RcxTrace tr = trace_rcx_addr(site);
    if (tr.deref_boundary) {
        log::info("[studio-system]   (deref boundary hit — cannot flatten to a simple base+disp slot)");
        return nullptr;
    }
    if (!tr.resolved || !tr.global) return nullptr;

    std::byte* base = nullptr;
    if (tr.global_load) {
        if (!safe_read(tr.global, base) || !base) return nullptr;
    } else {
        base = tr.global;
    }
    if (!is_readable(base, 8)) return nullptr;

    std::byte* slot = base + tr.disp;
    std::byte* studio = nullptr;
    if (!safe_read(slot, studio)) return nullptr;
    log::info("[studio-system]   handle slot 0x{:X} = Studio::System* 0x{:X}",
              reinterpret_cast<std::uintptr_t>(slot),
              reinterpret_cast<std::uintptr_t>(studio));
    return studio;
}

} // namespace

std::byte* locate_studio_system_handle(const PEImage& img) noexcept {
    if (!img.valid()) return nullptr;
    std::byte* create_fn = resolve_studio_anchor(img, "System::create");
    if (!create_fn) {
        log::info("[studio-system] System::create not uniquely resolved -> cannot locate handle");
        return nullptr;
    }

    // Pass 1: E8 rel32 direct calls.
    for (std::byte* p = img.text; p + 5 <= img.text_end; ++p) {
        if (p[0] != std::byte{0xE8}) continue;
        std::int32_t d = 0; std::memcpy(&d, p + 1, 4);
        if (p + 5 + d != create_fn) continue;
        log::info("[studio-system] create E8 xref at RVA 0x{:X}", static_cast<std::uintptr_t>(p - img.base));
        dump_hex(p - 48, 48, "ctx");
        std::byte* h = read_handle_from_site(p);
        if (h) return h;
    }

    // Pass 2: FF 15 [rip+disp32] RIP-indirect calls (via function pointer).
    for (std::byte* p = img.text; p + 6 <= img.text_end; ++p) {
        if (p[0] != std::byte{0xFF} || p[1] != std::byte{0x15}) continue;
        std::int32_t d = 0; std::memcpy(&d, p + 2, 4);
        std::byte* slot_p = p + 6 + d;
        std::byte* target = nullptr;
        if (!safe_read(slot_p, target) || target != create_fn) continue;
        log::info("[studio-system] create FF15 xref at RVA 0x{:X}", static_cast<std::uintptr_t>(p - img.base));
        dump_hex(p - 48, 48, "ctx");
        std::byte* h = read_handle_from_site(p);
        if (h) return h;
    }
    return nullptr;
}

void scout_studio_system(const PEImage& img) noexcept {
    if (!img.valid()) { log::warn("[studio-system] image invalid, skipping"); return; }
    log::info("[studio-system] ===== locate + verify Studio System handle =====");

    const char* anchors[] = {"System::create", "System::getCoreSystem",
                             "System::getBankCount", "System::getBankList"};
    std::byte* fns[4] = {};
    for (int i = 0; i < 4; ++i) fns[i] = resolve_studio_anchor(img, anchors[i]);
    log::info("[studio-system] create=0x{:X} getCoreSystem=0x{:X} getBankCount=0x{:X} getBankList=0x{:X}",
              reinterpret_cast<std::uintptr_t>(fns[0]),
              reinterpret_cast<std::uintptr_t>(fns[1]),
              reinterpret_cast<std::uintptr_t>(fns[2]),
              reinterpret_cast<std::uintptr_t>(fns[3]));

    // System::create may run a moment after DLL load (the game initializes FMOD
    // on the main thread). Retry briefly so a single startup capture usually
    // lands after the handle is populated.
    std::byte* studio = nullptr;
    for (int attempt = 0; attempt < 10 && !studio; ++attempt) {
        studio = locate_studio_system_handle(img);
        if (!studio) {
            log::info("[studio-system] handle not populated (attempt {}/10), waiting 1s", attempt + 1);
            std::this_thread::sleep_for(1s);
        }
    }
    if (!studio) {
        log::info("[studio-system] Studio System handle not obtained (create may not have run); retry at attach");
        return;
    }
    log::info("[studio-system] STUDIO SYSTEM HANDLE = 0x{:X}", reinterpret_cast<std::uintptr_t>(studio));

    // Identity: getCoreSystem -> core A (to be compared with resolve_fmod_system's
    // core B once a RadioStreamFmod is attached; FMOD Studio 2.03 C ABI:
    // RCX = FMOD_STUDIO_SYSTEM* system, RDX = FMOD_SYSTEM** coresystem).
    if (fns[1]) {
        using GetCoreSystem = std::uint32_t (*)(void*, void**);
        auto fn = reinterpret_cast<GetCoreSystem>(fns[1]);
        void* core = nullptr;
        std::uint32_t rc = 0xFFFFFFFF;
        const bool ok = seh_call([&] { rc = fn(studio, &core); });
        log::info("[studio-system] getCoreSystem rc={} core=0x{:X}{}", rc,
                  reinterpret_cast<std::uintptr_t>(core),
                  ok ? "" : " (seh-failed)");
    }

    // Liveness: getBankCount -> FMOD_OK + a reasonable count proves the handle
    // is a live Studio System. RCX = system, RDX = int* count.
    if (fns[2]) {
        using GetBankCount = std::uint32_t (*)(void*, std::int32_t*);
        auto fn = reinterpret_cast<GetBankCount>(fns[2]);
        std::int32_t count = -1;
        std::uint32_t rc = 0xFFFFFFFF;
        const bool ok = seh_call([&] { rc = fn(studio, &count); });
        const bool valid = ok && rc == 0 && count > 0;
        log::info("[studio-system] getBankCount rc={} count={}{}", rc, count,
                  valid ? " -> Studio System VALID" : "");
    }

    log::info("[studio-system] ===== done =====");
}

} // namespace fh6r::fmod
