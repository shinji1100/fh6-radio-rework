#include "fh6r/fmod/studio_param_hook.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace fh6r::fmod {
namespace {

// The shared logging/dispatch function that references every Studio name string.
constexpr std::uint8_t kDispatch[8] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57};

void dump_anchor(const PEImage& img, std::string_view anchor) {
    const auto cands = scout_anchor(img, anchor);
    std::size_t real = 0;
    log::info("[setparam] '{}' -> {} candidate(s) (read-only):", anchor, cands.size());
    for (auto* fn : cands) {
        std::uint8_t pre[8]{};
        const bool rd = is_readable(fn, sizeof(pre));
        if (rd) std::memcpy(pre, fn, sizeof(pre));
        const bool dispatch = rd && std::memcmp(pre, kDispatch, sizeof(pre)) == 0;
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
        if (!dispatch) ++real;
    }
    log::info("[setparam] '{}' -> {} real (non-dispatch) candidate(s)", anchor, real);
}

} // namespace

void install_studio_param_hooks(const PEImage& img) noexcept {
    if (!img.valid()) { log::warn("[setparam] image invalid, skip"); return; }
    log::info("[setparam] ===== studio parameter setter scout (READ-ONLY, no hooking) =====");
    dump_anchor(img, "System::setParameterByName");
    dump_anchor(img, "System::setParameterByID");
    log::info("[setparam] ===== done (nothing patched) =====");
}

} // namespace fh6r::fmod
