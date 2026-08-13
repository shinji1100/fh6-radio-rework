#include "fh6r/fmod/studio_probe.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fh6r::fmod {
namespace {

constexpr std::size_t kMaxVtableEntries = 64;
constexpr std::size_t kVtableDump = 48; // entries to log once found

// Find an 8-byte slot in a readable section that holds `fn` (a vtable entry).
std::byte* find_vtable_entry(const PEImage& img, std::byte* fn) noexcept {
    if (!fn) return nullptr;
    for (const auto& sec : img.sections) {
        if (!sec.readable() || sec.end <= sec.start + 8) continue;
        for (std::byte* p = sec.start; p + 8 <= sec.end; p += 8) {
            std::byte* v{};
            std::memcpy(&v, p, 8);
            if (v == fn) return p;
        }
    }
    return nullptr;
}

// Scan private committed memory for an object whose first 8 bytes == vtable.
bool heap_scan_object(const std::byte* vtable, std::byte** out_obj) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    std::byte* addr = nullptr;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        auto* region = reinterpret_cast<std::byte*>(mbi.BaseAddress);
        auto* end = region + mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY) &&
            !(mbi.Protect & PAGE_GUARD);
        if (readable && mbi.RegionSize <= 0x4000000) {
            for (std::byte* p = region; p + 8 <= end; p += 8) {
                std::byte* vt{};
                std::memcpy(&vt, p, 8);
                if (vt == vtable) { *out_obj = p; return true; }
            }
        }
        if (end <= addr) break;
        addr = end;
    }
    return false;
}

} // namespace

void* locate_studio_system(const PEImage& img, std::byte* method_fn) noexcept {
    auto* entry = find_vtable_entry(img, method_fn);
    if (!entry) {
        log::warn("[studio] vtable entry for method 0x{:X} not found in image",
                  reinterpret_cast<std::uintptr_t>(method_fn));
        return nullptr;
    }
    log::info("[studio] vtable entry at 0x{:X} holds method 0x{:X}",
              reinterpret_cast<std::uintptr_t>(entry),
              reinterpret_cast<std::uintptr_t>(method_fn));
    // The object's vtable pointer is the table START; the found entry is at some
    // offset into it. Walk candidate starts backward and heap-scan for each.
    for (std::size_t k = 0; k < kMaxVtableEntries; ++k) {
        const std::byte* vt = entry - k * 8;
        std::byte* obj = nullptr;
        if (heap_scan_object(vt, &obj)) {
            log::info("[studio] Studio::System object=0x{:X} vtable=0x{:X} (entry at +{}*8)",
                      reinterpret_cast<std::uintptr_t>(obj),
                      reinterpret_cast<std::uintptr_t>(vt), k);
            return obj;
        }
    }
    log::warn("[studio] no object found whose vtable contains the method entry");
    return nullptr;
}

void studio_scout(const PEImage& img) noexcept {
    if (!img.valid()) { log::warn("[studio] image invalid, skipping"); return; }
    log::info("[studio] ===== Studio System scout start =====");

    // Resolve the Studio getters/accessors we need (dispatch filtered).
    auto* get_param_desc_list = resolve_studio_anchor(img, "System::getParameterDescriptionList");
    auto* get_param_desc_count = resolve_studio_anchor(img, "System::getParameterDescriptionCount");
    auto* get_param_by_id = resolve_studio_anchor(img, "System::getParameterByID");
    auto* get_param_by_name = resolve_studio_anchor(img, "System::getParameterByName");
    log::info("[studio] getParameterDescriptionList=0x{:X} count=0x{:X} getByID=0x{:X} getByName=0x{:X}",
              reinterpret_cast<std::uintptr_t>(get_param_desc_list),
              reinterpret_cast<std::uintptr_t>(get_param_desc_count),
              reinterpret_cast<std::uintptr_t>(get_param_by_id),
              reinterpret_cast<std::uintptr_t>(get_param_by_name));

    if (!get_param_desc_list) {
        log::warn("[studio] cannot locate Studio::System (getParameterDescriptionList unresolved)");
        log::info("[studio] ===== Studio System scout done (failed) =====");
        return;
    }

    void* studio = locate_studio_system(img, get_param_desc_list);
    if (!studio) {
        log::info("[studio] ===== Studio System scout done (not found) =====");
        return;
    }

    // Dump the vtable so the next iteration can resolve Studio methods by their
    // stable vtable offset instead of the anchor+dispatch-filter path.
    std::byte* vtable = nullptr;
    if (!seh_call([&] { std::memcpy(&vtable, studio, 8); }) || !vtable) {
        log::warn("[studio] could not read vtable pointer from object");
        return;
    }
    log::info("[studio] vtable dump ({} entries):", kVtableDump);
    for (std::size_t i = 0; i < kVtableDump; ++i) {
        std::byte* fn = nullptr;
        if (!seh_call([&] { std::memcpy(&fn, vtable + i * 8, 8); })) break;
        log::info("[studio]   [{}] 0x{:X}", i, reinterpret_cast<std::uintptr_t>(fn));
    }
    log::info("[studio] ===== Studio System scout done =====");
}

} // namespace fh6r::fmod
