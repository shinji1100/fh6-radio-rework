#include "fh6r/fmod/studio_api_scout.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace fh6r::fmod {
namespace {

// The 13 Studio APIs for the public enumeration route. Order roughly follows
// the Step-4 traversal: System -> Bank -> EventDescription -> active
// EventInstance. The three highest-priority anchors come first.
constexpr std::string_view kApis[] = {
    "System::getEvent",
    "EventDescription::getInstanceCount",
    "EventDescription::getInstanceList",
    "System::getBankCount",
    "System::getBankList",
    "Bank::getEventCount",
    "Bank::getEventList",
    "EventDescription::is3D",
    "EventDescription::isStream",
    "EventDescription::getPath",
    "EventDescription::getMinMaxDistance",
    "EventDescription::getSoundSize",
    "EventDescription::createInstance",
};

} // namespace

void scout_studio_api(const PEImage& img) noexcept {
    if (!img.valid()) { log::warn("[studio-api] image invalid, skipping"); return; }
    log::info("[studio-api] ===== Studio API anchor scout (resolve only, no calls) =====");

    for (const auto anchor : kApis) {
        const auto real = scout_studio_anchor(img, anchor);
        const char* status = real.empty() ? "MISSING"
                           : (real.size() == 1 ? "RESOLVED" : "AMBIGUOUS");
        log::info("[studio-api] {} candidates={} status={}", anchor, real.size(), status);
        for (auto* fn : real)
            log::info("[studio-api]   addr=0x{:X}", reinterpret_cast<std::uintptr_t>(fn));

        // Diagnose non-RESOLVED anchors in one round: dump the raw candidate
        // prologues (before dispatch filtering) so we can tell whether the
        // shared-dispatch filter still applies to this API class.
        if (real.size() != 1) {
            const auto raw = scout_anchor(img, anchor);
            for (auto* fn : raw) {
                std::uint8_t pre[8]{};
                if (is_readable(fn, sizeof(pre))) std::memcpy(pre, fn, sizeof(pre));
                log::info("[studio-api]   raw-cand 0x{:X} prologue=[{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}]",
                          reinterpret_cast<std::uintptr_t>(fn),
                          pre[0], pre[1], pre[2], pre[3], pre[4], pre[5], pre[6], pre[7]);
            }
        }
    }
    log::info("[studio-api] ===== done =====");
}

} // namespace fh6r::fmod
