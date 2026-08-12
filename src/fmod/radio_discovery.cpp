#include "fh6r/fmod/radio_discovery.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>

namespace fh6r::fmod {
namespace {
bool in_image(const PEImage& img, const void* p) noexcept {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    const auto begin = reinterpret_cast<std::uintptr_t>(img.base);
    return v >= begin && v < begin + img.size;
}
const std::byte* find_typedesc_in(const std::byte* s, const std::byte* e) noexcept {
    constexpr std::string_view leaf = "RadioStreamFmod";
    constexpr std::string_view outer = "_Ref_count_obj2";
    if (!s || !e || static_cast<std::size_t>(e - s) < leaf.size() + 16) return nullptr;
    for (const std::byte* p = s; p + leaf.size() < e; ++p) {
        if (std::memcmp(p, leaf.data(), leaf.size()) != 0) continue;
        const std::byte* lo = (p - s > 128) ? p - 128 : s;
        bool has_outer = false;
        for (const std::byte* q = lo; q + outer.size() <= p; ++q) {
            if (std::memcmp(q, outer.data(), outer.size()) == 0) { has_outer = true; break; }
        }
        if (!has_outer) continue;
        for (const std::byte* q = p; q - s >= 4; --q) {
            if (q[-2] == std::byte{'.'} && q[-1] == std::byte{'?'} && q[0] == std::byte{'A'} && q[1] == std::byte{'V'})
                return (q - 2 - s >= 16) ? q - 18 : nullptr;
        }
    }
    return nullptr;
}
const std::byte* find_typedesc(const PEImage& img) noexcept {
    for (const auto& sec : img.sections) if (sec.readable()) if (auto* x = find_typedesc_in(sec.start, sec.end)) return x;
    return nullptr;
}
const std::byte* find_col(const PEImage& img, std::uint32_t td_rva) noexcept {
    for (const auto& sec : img.sections) {
        if (!sec.readable() || sec.end <= sec.start + 24) continue;
        for (const std::byte* p = sec.start; p + 16 <= sec.end; p += 4) {
            std::uint32_t sig{}, td{}; std::memcpy(&sig, p, 4); std::memcpy(&td, p + 12, 4);
            if (sig == 1 && td == td_rva) return p;
        }
    }
    return nullptr;
}
std::vector<std::byte*> find_vtables(const std::byte* s, const std::byte* e, const std::byte* col) {
    std::vector<std::byte*> out;
    for (const std::byte* p = s; p + 8 <= e; p += 8) {
        const std::byte* v{}; std::memcpy(&v, p, 8);
        if (v == col) out.push_back(const_cast<std::byte*>(p) + 8);
    }
    return out;
}
constexpr std::size_t kMaxHits = 64;
void scan_region(const PEImage& img, const std::byte* vtable, std::byte* p, std::byte* end,
                 std::vector<std::byte*>& out) noexcept {
    seh_call([&] {
        for (; p + 24 <= end && out.size() < kMaxHits; p += 16) {
            std::byte* vt{}; std::memcpy(&vt, p, 8); if (vt != vtable) continue;
            std::uint32_t use{}, weak{}; std::memcpy(&use, p + 8, 4); std::memcpy(&weak, p + 12, 4);
            if (!use || !weak || use > 0x80 || weak > 0x80) continue;
            std::byte* inner{}; std::memcpy(&inner, p + 16, 8);
            if (!in_image(img, inner)) continue;
            out.push_back(p);
        }
    });
}
std::vector<std::byte*> scan_heap(const PEImage& img, const std::byte* vtable) noexcept {
    std::vector<std::byte*> out;
    MEMORY_BASIC_INFORMATION mbi{};
    std::byte* addr = nullptr;
    const std::byte* prev_end = nullptr;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi) && out.size() < kMaxHits) {
        auto* region = reinterpret_cast<std::byte*>(mbi.BaseAddress);
        auto* end = region + mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY || mbi.Protect == PAGE_READONLY) &&
            !(mbi.Protect & PAGE_GUARD);
        const bool module = in_image(img, region);
        if (readable && mbi.RegionSize <= 0x4000000 && !module) {
            auto* p = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(region + 15)) & ~std::uintptr_t{15});
            auto* aend = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(end)) & ~std::uintptr_t{7});
            scan_region(img, vtable, p, aend, out);
        }
        if (end <= prev_end) break;
        prev_end = end; addr = end;
    }
    return out;
}
std::string read_sound_name(std::byte* stream, int* step, std::byte** body) noexcept {
    if (step) *step = 0;
    if (body) *body = nullptr;
    std::byte* a{}; if (!safe_read(stream + 0x48, a) || !a) return {};
    if (step) *step = 1;
    std::byte* b{}; if (!safe_read(a + 0x18, b) || !b) return {};
    if (step) *step = 2;
    if (body) *body = b;
    std::string out;
    seh_call([&] { if (auto s = safe_read_msvc_string(b + 0x10)) out = std::move(*s); });
    return out;
}
struct Cache {
    bool located = false;
    const std::byte* typedesc = nullptr;
    const std::byte* col = nullptr;
    std::byte* vtable = nullptr;
    std::vector<std::byte*> candidates;
    int empty_streak = 0;
};
std::mutex g_mu;
Cache g_cache;
constexpr int kRescanThreshold = 20;
}

void* resolve_fmod_system(const PEImage& img, std::byte* stream) noexcept {
    void* out = nullptr;
    if (!stream) return out;
    seh_call([&] {
        std::byte* x{}; if (!safe_read(stream + 0x08, x) || !x) return;
        std::byte* sys{}; if (!safe_read(x + 0xC0, sys) || !sys) return;
        std::byte* vt{}; if (!safe_read(sys, vt) || !in_image(img, vt)) return;
        out = sys;
    });
    return out;
}

DiscoveryResult discover_radio_instances(const PEImage& img) noexcept {
    DiscoveryResult result;
    if (!img.valid()) return result;
    Cache local;
    { std::scoped_lock lk{g_mu}; local = g_cache; }
    if (!local.located) {
        const auto* td = find_typedesc(img);
        if (!td) return result;
        const auto td_rva = static_cast<std::uint32_t>(td - img.base);
        const auto* col = find_col(img, td_rva);
        if (!col) return result;
        std::byte* found_vt = nullptr;
        std::vector<std::byte*> found;
        for (int pass = 0; pass < 2 && found.empty(); ++pass) {
            const std::byte* s = pass == 0 ? img.rdata : img.base;
            const std::byte* e = pass == 0 ? img.rdata_end : img.base + img.size;
            for (auto* vt : find_vtables(s, e, col)) {
                std::byte* first{}; std::memcpy(&first, vt, 8);
                if (!in_image(img, first)) continue;
                auto hits = scan_heap(img, vt);
                if (!hits.empty()) { found_vt = vt; found = std::move(hits); break; }
            }
        }
        if (found.empty()) return result;
        local.located = true; local.typedesc = td; local.col = col; local.vtable = found_vt; local.candidates = std::move(found);
        { std::scoped_lock lk{g_mu}; g_cache = local; }
        log::info("[radio] cached {} RadioStreamFmod candidate(s)", local.candidates.size());
    }
    for (auto* rc : local.candidates) {
        std::byte* body{}; int step = 0;
        auto* stream = rc + 16;
        auto name = read_sound_name(stream, &step, &body);
        if (!name.empty()) result.instances.push_back({rc, stream, body, std::move(name)});
    }
    result.vtable = local.vtable;
    if (result.instances.empty()) {
        local.empty_streak++;
        std::scoped_lock lk{g_mu};
        if (local.empty_streak >= kRescanThreshold) g_cache = Cache{};
        else g_cache.empty_streak = local.empty_streak;
    } else {
        std::scoped_lock lk{g_mu}; g_cache.empty_streak = 0;
    }
    return result;
}
} // namespace fh6r::fmod
