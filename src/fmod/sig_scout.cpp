#include "fh6r/fmod/sig_scout.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

namespace fh6r::fmod {
namespace {

struct Desired { std::string_view anchor; std::string_view label; };

// Low-level FMOD API surface needed by the Phase 1 audio-state enumerator.
// NOTE (static analysis finding): in this FH6 build the FMOD low-level code
// is encrypted on disk and decrypted at load by the game loader, so these
// signatures are only resolvable in the LIVE process -- which is exactly where
// this scout runs. The four "GROUND TRUTH" entries must resolve (the existing
// plugin already does) and serve as the cross-validation baseline.
// Trimmed to only the APIs Phase 1.1 actually needs; on-disk-absent variants
// are dropped (they are absent at runtime too, since .rdata is not encrypted).
constexpr Desired kLowLevel[] = {
    {"System::createDSP",                  "createDSP (GROUND TRUTH)"},
    {"DSP::release",                       "release (GROUND TRUTH)"},
    {"ChannelControl::addDSP",             "addDSP (GROUND TRUTH)"},
    {"ChannelControl::removeDSP",          "removeDSP (GROUND TRUTH)"},
    {"System::get3DListenerAttributes",    "listener attrs"},
    {"ChannelControl::getNumDSPs",         "num DSPs"},
    {"ChannelControl::getDSP",             "get DSP"},
    {"DSP::getType",                       "DSP type"},
    {"DSP::getNumParameters",              "DSP num params"},
    {"DSP::getParameterFloat",             "DSP param float"},
    {"DSP::getParameterInt",               "DSP param int"},
    {"DSP::getInfo",                       "DSP info"},
    {"DSP::getNumInputs",                  "DSP num inputs"},
    {"System::getMasterChannelGroup",      "master channel group"},
    {"ChannelGroup::getNumGroups",         "group num groups"},
    {"ChannelGroup::getGroup",             "group get group"},
    {"DSP::getParameterData",              "DSP param data"},
};

// Studio API: confirmed present AND LEA-referenced on disk in this build.
// Trimmed to the subset relevant to interior/exterior state acquisition:
// global parameter enumeration (the preferred signal source) + listener +
// bus state. Stored as "System::<method>" / "Bus::<method>" (no "Studio::"
// prefix) -- the FMOD debug name table drops the namespace.
constexpr Desired kStudio[] = {
    {"System::getParameterDescriptionList",  "global param list"},
    {"System::getParameterDescriptionCount", "global param count"},
    {"System::getParameterByID",             "get param by id"},
    {"System::getParameterByName",           "get param by name"},
    {"System::getParameterByNameWithLabel",  "get param by name+label"},
    {"System::setListenerAttributes",        "set listener attrs"},
    {"System::setNumListeners",             "set num listeners"},
    {"System::setListenerWeight",           "set listener weight"},
    {"System::setParameterByID",            "set param by id"},
    {"System::setParameterByName",          "set param by name"},
    {"Bus::getChannelGroup",                "bus channel group"},
    {"Bus::setVolume",                      "bus set volume"},
    {"Bus::setMute",                        "bus set mute"},
};

constexpr std::size_t kMaxShown = 8;
constexpr std::size_t kPrologueBytes = 32;

std::string hex_bytes(const std::byte* p, std::size_t n) {
    std::string s;
    s.reserve(n * 3);
    char buf[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), i ? " %02X" : "%02X",
                      static_cast<unsigned>(std::to_integer<unsigned>(p[i])));
        s += buf;
    }
    return s;
}

void scout_group(std::string_view tag, const Desired* list, std::size_t count,
                 const PEImage& img) noexcept {
    log::info("[scout] --- {} ---", tag);
    for (std::size_t k = 0; k < count; ++k) {
        const auto& d = list[k];
        const auto cands = scout_anchor(img, d.anchor);
        if (cands.empty()) {
            log::warn("[scout] [{}] '{}' ({}) not found", tag, d.anchor, d.label);
            continue;
        }
        log::info("[scout] [{}] '{}' ({}) -> {} candidate(s)",
                  tag, d.anchor, d.label, cands.size());
        std::size_t shown = 0;
        for (auto* fn : cands) {
            if (shown >= kMaxShown) {
                log::info("[scout]   ...{} more (capped at {})", cands.size() - shown, kMaxShown);
                break;
            }
            std::array<std::byte, kPrologueBytes> prologue{};
            const bool readable = is_readable(fn, prologue.size());
            if (readable) std::memcpy(prologue.data(), fn, prologue.size());
            log::info("[scout]   #{} fn=0x{:X} prologue=[{}]",
                      shown,
                      reinterpret_cast<std::uintptr_t>(fn),
                      readable ? hex_bytes(prologue.data(), prologue.size())
                               : std::string("unreadable"));
            ++shown;
        }
    }
}

} // namespace

void scout_apis(const PEImage& img) noexcept {
    log::info("[scout] ===== FMOD API signature scout start =====");
    scout_group("lowlevel", kLowLevel, std::size(kLowLevel), img);
    scout_group("studio", kStudio, std::size(kStudio), img);
    log::info("[scout] ===== FMOD API signature scout done =====");
}

} // namespace fh6r::fmod
