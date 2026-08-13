#include "fh6r/fmod/studio_param_hook.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace fh6r::fmod {
namespace {

// FMOD Studio setter ABI (x64): RCX=this, RDX=id, XMM2=value, R9=ignoreSeekSpeed.
// FMOD_STUDIO_PARAMETER_ID = { uint32 data1; uint32 data2; } -> one u64 in RDX.
using SetParamByIDFn = std::uint32_t (*)(void*, std::uint64_t, float, bool);

SetParamByIDFn g_orig_by_id = nullptr;

// The shared logging/dispatch function that references every Studio name string.
constexpr std::uint8_t kDispatch[8] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57};

// Resolve a Studio method to a unique address, filtering the dispatch function.
// Returns nullptr when there is not exactly one real candidate.
std::byte* resolve_unique(const PEImage& img, std::string_view anchor) {
    const auto cands = scout_anchor(img, anchor);
    std::byte* real = nullptr;
    int count = 0;
    for (auto* fn : cands) {
        std::uint8_t pre[8]{};
        const bool rd = is_readable(fn, sizeof(pre));
        if (rd) std::memcpy(pre, fn, sizeof(pre));
        if (rd && std::memcmp(pre, kDispatch, sizeof(pre)) == 0) continue;
        real = fn;
        ++count;
    }
    if (count != 1) {
        log::warn("[setparam] '{}' -> {} real candidate(s) (need 1)", anchor, count);
        return nullptr;
    }
    return real;
}

// Every 8-byte slot in a readable image section that holds `fn` (candidate
// vtable entries / pointer tables referencing the function).
std::vector<std::byte*> find_slots(const PEImage& img, std::byte* fn) {
    std::vector<std::byte*> slots;
    if (!fn) return slots;
    for (const auto& sec : img.sections) {
        if (!sec.readable() || sec.end <= sec.start + 8) continue;
        for (std::byte* p = sec.start; p + 8 <= sec.end; p += 8) {
            std::byte* v{};
            std::memcpy(&v, p, 8);
            if (v == fn) slots.push_back(p);
        }
    }
    return slots;
}

// True if `p` looks like a code pointer into the image's .text (used to confirm
// a slot is a vtable entry surrounded by other function pointers).
bool is_text_ptr(const PEImage& img, const std::byte* p) noexcept {
    return p >= img.text && p < img.text_end;
}

// Overwrite an 8-byte slot (make it writable, write, restore protection).
bool patch_slot(std::byte* slot, void* hook) noexcept {
    if (!slot || !hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
    std::memcpy(slot, &hook, 8);
    VirtualProtect(slot, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, 8);
    return true;
}

// Dedupe cache: log an (id, value) pair only on first sight or value change, so
// a stationary car does not flood the log. Not thread-safe on purpose (probe
// only); worst case a change is logged twice.
constexpr int kSeenCap = 128;
struct Seen { std::uint64_t id; float last; bool used; };
Seen g_seen[kSeenCap]{};

std::uint32_t hook_set_param_by_id(void* sys, std::uint64_t id, float value,
                                   bool ignore_seek) noexcept {
    bool changed = false;
    int empty = -1;
    for (int i = 0; i < kSeenCap; ++i) {
        if (g_seen[i].used && g_seen[i].id == id) {
            changed = (value > g_seen[i].last + 0.0005f) ||
                      (value < g_seen[i].last - 0.0005f);
            g_seen[i].last = value;
            empty = -2; // found
            break;
        }
        if (!g_seen[i].used && empty < 0) empty = i;
    }
    if (empty >= 0) { // new id
        g_seen[empty].used = true;
        g_seen[empty].id = id;
        g_seen[empty].last = value;
        changed = true;
    }
    if (changed) {
        log::info("[setparam] id={:08X}:{:08X} = {:.4f}",
                  static_cast<unsigned>(id & 0xFFFFFFFFu),
                  static_cast<unsigned>(id >> 32), value);
    }
    return g_orig_by_id(sys, id, value, ignore_seek);
}

} // namespace

void install_studio_param_hooks(const PEImage& img) noexcept {
    if (!img.valid()) { log::warn("[setparam] image invalid, skip"); return; }
    log::info("[setparam] ===== studio parameter hook install (vtable patch) =====");

    std::byte* by_id = resolve_unique(img, "System::setParameterByID");
    if (!by_id) {
        log::warn("[setparam] setParameterByID not uniquely resolved -> not hooking");
    } else {
        const auto slots = find_slots(img, by_id);
        log::info("[setparam] setParameterByID=0x{:X} -> {} slot(s) in image",
                  reinterpret_cast<std::uintptr_t>(by_id), slots.size());
        for (auto* s : slots)
            log::info("[setparam]   slot 0x{:X}", reinterpret_cast<std::uintptr_t>(s));

        std::byte* chosen = nullptr;
        if (slots.size() == 1) {
            // Confirm it is a vtable entry: at least one adjacent 8-byte word is
            // also a code pointer (a vtable is a contiguous run of function ptrs).
            std::byte* n1 = nullptr;
            std::byte* n2 = nullptr;
            if (is_readable(slots[0] - 8, 8)) std::memcpy(&n1, slots[0] - 8, 8);
            if (is_readable(slots[0] + 8, 8)) std::memcpy(&n2, slots[0] + 8, 8);
            if (is_text_ptr(img, n1) || is_text_ptr(img, n2))
                chosen = slots[0];
            else
                log::warn("[setparam] slot neighbours are not code ptrs -> not a vtable, skip");
        }

        if (chosen) {
            g_orig_by_id = reinterpret_cast<SetParamByIDFn>(by_id); // original, unpatched
            if (patch_slot(chosen, reinterpret_cast<void*>(&hook_set_param_by_id)))
                log::info("[setparam] setParameterByID HOOKED via vtable slot 0x{:X}",
                          reinterpret_cast<std::uintptr_t>(chosen));
            else
                log::warn("[setparam] setParameterByID vtable patch failed");
        } else {
            log::warn("[setparam] setParameterByID: not hooking (ambiguous/unconfirmed slot)");
        }
    }

    // setParameterByName has 3 real candidates in this build (overloads) — log
    // them read-only for a future attempt, do not hook.
    {
        const auto cands = scout_anchor(img, "System::setParameterByName");
        log::info("[setparam] setParameterByName -> {} candidate(s) (read-only):", cands.size());
        for (auto* fn : cands) {
            std::uint8_t pre[8]{};
            const bool rd = is_readable(fn, sizeof(pre));
            if (rd) std::memcpy(pre, fn, sizeof(pre));
            const bool dispatch = rd && std::memcmp(pre, kDispatch, sizeof(pre)) == 0;
            log::info("[setparam]   0x{:X}{}",
                      reinterpret_cast<std::uintptr_t>(fn), dispatch ? "  [DISPATCH]" : "");
        }
    }

    log::info("[setparam] ===== install done (byId={}) =====",
              g_orig_by_id ? "hooked" : "skipped");
}

} // namespace fh6r::fmod
