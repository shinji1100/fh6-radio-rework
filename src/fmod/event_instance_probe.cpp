#include "fh6r/fmod/event_instance_probe.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <cstdint>
#include <cstring>

namespace fh6r::fmod {
namespace {

struct Method {
    const char* anchor;
    const char* label;
};

// EventInstance C API names resolved only to observe the wrapper's vtable.
// These are observation markers, NOT identity proof: FMOD Studio API types are
// packed handles, not vtable-polymorphic objects, so a vtable here is more
// consistent with an FH6 wrapper than with an EventInstance itself.
constexpr Method kMethods[] = {
    {"EventInstance::getChannelGroup",  "getChannelGroup"},
    {"EventInstance::getDescription",   "getDescription"},
    {"EventInstance::getPlaybackState", "getPlaybackState"},
    {"EventInstance::getVolume",        "getVolume"},
    {"EventInstance::getPaused",        "getPaused"},
};
constexpr std::size_t kMethodCount = sizeof(kMethods) / sizeof(kMethods[0]);
constexpr std::size_t kMaxEntries = 64;

} // namespace

bool probe_event_instance(const PEImage& img, std::byte* radio_stream) noexcept {
    if (!img.valid() || !radio_stream) {
        log::warn("[event] probe skipped (img_valid={} stream={})",
                  img.valid(), radio_stream != nullptr);
        return false;
    }

    // Resolve EventInstance methods (dispatch-filtered). A nullptr means that
    // anchor did not resolve uniquely in this build — log it and keep matching
    // against the ones that did.
    std::byte* fn[kMethodCount]{};
    for (std::size_t i = 0; i < kMethodCount; ++i)
        fn[i] = resolve_studio_anchor(img, kMethods[i].anchor);

    log::info("[event] EventInstance methods:");
    for (std::size_t i = 0; i < kMethodCount; ++i)
        log::info("[event]   {} = 0x{:X}", kMethods[i].label,
                  reinterpret_cast<std::uintptr_t>(fn[i]));

    // First 8 bytes of the object = vtable pointer.
    std::byte* vtable = nullptr;
    if (!seh_call([&] { std::memcpy(&vtable, radio_stream, 8); }) || !vtable) {
        log::warn("[event] radio_stream=0x{:X}: no readable vtable",
                  reinterpret_cast<std::uintptr_t>(radio_stream));
        return false;
    }

    log::info("[event] radio_stream=0x{:X} vtable=0x{:X} (dump {} entries):",
              reinterpret_cast<std::uintptr_t>(radio_stream),
              reinterpret_cast<std::uintptr_t>(vtable), kMaxEntries);

    int matches = 0;
    for (std::size_t i = 0; i < kMaxEntries; ++i) {
        std::byte* entry = nullptr;
        if (!seh_call([&] { std::memcpy(&entry, vtable + i * 8, 8); })) break;
        const char* tag = nullptr;
        for (std::size_t m = 0; m < kMethodCount; ++m) {
            if (entry == fn[m]) { tag = kMethods[m].label; ++matches; break; }
        }
        if (tag)
            log::info("[event]   [{:2}] 0x{:X}  <{}>", i,
                      reinterpret_cast<std::uintptr_t>(entry), tag);
        else
            log::info("[event]   [{:2}] 0x{:X}", i, reinterpret_cast<std::uintptr_t>(entry));
    }

    // Observation only. Do NOT interpret matches>=2 as "is an EventInstance":
    // FMOD Studio types are packed handles, so this vtable is expected to belong
    // to an FH6 wrapper, not the EventInstance itself. The dump is a data point
    // for locating the wrapper field that holds the real EventInstance handle.
    log::info("[event] vtable match count={} (observation only; identity NOT asserted)", matches);
    return true;
}

} // namespace fh6r::fmod
