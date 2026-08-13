#include "fh6r/fmod/radio_acoustics_probe.hpp"

#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/fmod/studio_system_scout.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fh6r::fmod {
namespace {

constexpr std::uint32_t kMode2D = 0x00000008;
constexpr std::uint32_t kMode3D = 0x00000010;
constexpr std::uint32_t kCreateStream = 0x00000080;

using EventGetPlaybackState = std::uint32_t (*)(void*, std::int32_t*);
using EventDescBool = std::uint32_t (*)(void*, std::int32_t*);
using GetDescriptionThunk = std::uint64_t (*)(std::uint64_t*);

// FH6 wrapper at RVA 0x31D16B0 in the current build. It accepts a pointer to
// an EventInstance packed handle, calls the real getDescription implementation,
// and returns the description handle. The complete small thunk is matched;
// find_by_pattern requires it to be unique before we ever call it.
constexpr const char* kGetDescriptionThunk =
    "48 83 EC 28 48 8B 09 48 C7 44 24 30 00 00 00 00 "
    "48 85 C9 74 ?? 48 8D 54 24 30 E8 ?? ?? ?? ?? "
    "48 8B 44 24 30 48 83 C4 28 C3";
constexpr const char* kGetPlaybackState =
    "48 89 5C 24 18 55 56 57 48 81 EC 60 01 00 00 "
    "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 50 01 00 00 "
    "48 8B F2 48 8B E9 48 85 D2";

struct HandleCandidate {
    const char* source = nullptr;
    std::uint64_t value = 0;
};

bool read_qword(std::byte* base, std::ptrdiff_t offset, std::uint64_t& value) noexcept {
    value = 0;
    return base && safe_read(base + offset, value);
}

} // namespace

void probe_radio_acoustics(const PEImage& img, const RadioInstance& radio,
                           const FMODFns& fns) noexcept {
    if (!img.valid() || !radio.radio_stream) {
        log::warn("[radio-acoustics] skipped: invalid image/stream");
        return;
    }

    log::info("[radio-acoustics] ===== active Streamer Mode identity probe =====");

    std::uint64_t wrapper_sound = 0;
    std::uint64_t wrapper_studio = 0;
    std::uint64_t wrapper_channel = 0;
    read_qword(radio.radio_stream, 0x10, wrapper_sound);
    read_qword(radio.radio_stream, 0x18, wrapper_studio);
    read_qword(radio.radio_stream, 0x20, wrapper_channel);
    log::info("[radio-acoustics] wrapper stream=0x{:X} event_or_sound_handle=0x{:X} studio_handle=0x{:X} channel_handle=0x{:X}",
              reinterpret_cast<std::uintptr_t>(radio.radio_stream), wrapper_sound,
              wrapper_studio, wrapper_channel);

    const auto studio = studio_system_snapshot();
    if (studio.valid) {
        const auto verified = reinterpret_cast<std::uintptr_t>(studio.handle);
        log::info("[radio-acoustics] wrapper Studio handle 0x{:X} vs verified 0x{:X} -> {}",
                  wrapper_studio, verified,
                  wrapper_studio == verified ? "MATCH [PROVEN]" : "MISMATCH");
    }

    std::uint64_t current_sound = 0;
    std::uint32_t current_rc = ~0u;
    bool current_ok = false;
    if (fns.channel_get_current_sound && wrapper_channel) {
        current_ok = seh_call([&] {
            current_rc = fns.channel_get_current_sound(wrapper_channel, &current_sound);
        });
    }
    const auto discovered_sound = reinterpret_cast<std::uint64_t>(radio.fmod_sound);
    log::info("[radio-acoustics] Channel::getCurrentSound rc={} sound=0x{:X} seh_ok={} radio_discovery_object=0x{:X} packed_handle=0x{:X}",
              current_rc, current_sound, current_ok, discovered_sound, wrapper_sound);

    std::uint32_t mode = 0;
    std::uint32_t mode_rc = ~0u;
    bool mode_ok = false;
    const auto sound = current_rc == 0 && current_sound ? current_sound : wrapper_sound;
    if (fns.sound_get_mode && sound) {
        mode_ok = seh_call([&] { mode_rc = fns.sound_get_mode(sound, &mode); });
    }
    if (mode_ok && mode_rc == 0) {
        log::info("[radio-acoustics] Sound::getMode rc=0 mode=0x{:08X} is2D={} is3D={} createStream={}",
                  mode, (mode & kMode2D) != 0, (mode & kMode3D) != 0,
                  (mode & kCreateStream) != 0);
    } else {
        log::warn("[radio-acoustics] Sound::getMode rc={} seh_ok={}", mode_rc, mode_ok);
    }

    auto get_playback = reinterpret_cast<EventGetPlaybackState>(
        find_by_anchor(img, "EventInstance::getPlaybackState", kGetPlaybackState));
    auto get_description = reinterpret_cast<GetDescriptionThunk>(
        find_by_pattern(img, kGetDescriptionThunk));
    auto is_3d = reinterpret_cast<EventDescBool>(
        resolve_studio_anchor(img, "EventDescription::is3D"));
    auto is_stream = reinterpret_cast<EventDescBool>(
        resolve_studio_anchor(img, "EventDescription::isStream"));
    log::info("[radio-acoustics] Studio getters playback=0x{:X} getDescriptionThunk=0x{:X} is3D=0x{:X} isStream=0x{:X}",
              reinterpret_cast<std::uintptr_t>(get_playback),
              reinterpret_cast<std::uintptr_t>(get_description),
              reinterpret_cast<std::uintptr_t>(is_3d),
              reinterpret_cast<std::uintptr_t>(is_stream));

    std::uint64_t fmod_object_word = 0;
    if (radio.fmod_sound) read_qword(radio.fmod_sound, 0x08, fmod_object_word);
    const std::array candidates{
        HandleCandidate{"stream+0x10(event-or-sound)", wrapper_sound},
        HandleCandidate{"stream+0x18(studio)", wrapper_studio},
        HandleCandidate{"stream+0x20(channel)", wrapper_channel},
        HandleCandidate{"fmod_sound+0x08", fmod_object_word},
    };

    std::uint64_t event_handle = 0;
    int event_matches = 0;
    if (get_playback) {
        for (const auto& candidate : candidates) {
            if (!candidate.value) continue;
            std::int32_t state = -1;
            std::uint32_t rc = ~0u;
            const bool ok = seh_call([&] {
                rc = get_playback(reinterpret_cast<void*>(candidate.value), &state);
            });
            log::info("[radio-acoustics] EventInstance candidate {}=0x{:X} playback_rc={} state={} seh_ok={}",
                      candidate.source, candidate.value, rc, state, ok);
            if (ok && rc == 0 && state >= 0 && state <= 3) {
                event_handle = candidate.value;
                ++event_matches;
            }
        }
    }

    bool direct_event_proven = false;
    bool direct_event_3d = false;
    bool direct_event_stream = false;
    if (event_matches == 1 && get_description && is_3d && is_stream) {
        std::uint64_t input = event_handle;
        std::uint64_t description = 0;
        const bool desc_ok = seh_call([&] { description = get_description(&input); });
        // FMOD_BOOL is an int in the public ABI, although optimized wrappers
        // may clear only its low byte on entry. Zero-initialize the full word.
        std::int32_t three_d = 0;
        std::int32_t streaming = 0;
        std::uint32_t rc_3d = ~0u;
        std::uint32_t rc_stream = ~0u;
        const bool call_3d = desc_ok && description && seh_call([&] {
            rc_3d = is_3d(reinterpret_cast<void*>(description), &three_d);
        });
        const bool call_stream = desc_ok && description && seh_call([&] {
            rc_stream = is_stream(reinterpret_cast<void*>(description), &streaming);
        });
        log::info("[radio-acoustics] EventInstance=0x{:X} description=0x{:X} desc_ok={} is3D_rc={} is3D={} seh_ok={} isStream_rc={} isStream={} seh_ok={}",
                  event_handle, description, desc_ok, rc_3d, three_d, call_3d,
                  rc_stream, streaming, call_stream);
        if (call_3d && call_stream && rc_3d == 0 && rc_stream == 0) {
            direct_event_proven = true;
            direct_event_3d = three_d != 0;
            direct_event_stream = streaming != 0;
            log::info("[radio-acoustics] direct Radio Event identity -> CONFIRMED is3D={} isStream={} [PROVEN]",
                      direct_event_3d, direct_event_stream);
        }
    } else if (event_matches == 0) {
        log::info("[radio-acoustics] no EventInstance among verified direct wrapper handle fields; Core mode result remains authoritative for the injected stream (not a global absence proof)");
    } else if (event_matches > 1) {
        log::warn("[radio-acoustics] {} EventInstance-looking handles; refusing ambiguous identity", event_matches);
    }

    if (current_ok && current_rc == 0 && current_sound && mode_ok && mode_rc == 0) {
        const char* dimension = (mode & kMode3D) ? "3D" : ((mode & kMode2D) ? "2D" : "dimension-unknown");
        const char* storage = (mode & kCreateStream) ? "STREAM" : "SAMPLE";
        if (direct_event_proven) {
            log::info("[radio-acoustics] DECISION: active Core Sound={} {} [PROVEN]; direct Studio Event is3D={} isStream={} [PROVEN]; {}",
                      dimension, storage, direct_event_3d, direct_event_stream,
                      (mode & kMode3D)
                          ? "native 3D routing remains a candidate"
                          : "use the custom cabin DSP for interior/exterior acoustics");
        } else {
            log::info("[radio-acoustics] DECISION: active Core Sound={} {} [PROVEN]; direct Studio Event identity unresolved; {}",
                      dimension, storage,
                      (mode & kMode3D)
                          ? "native 3D routing remains a candidate"
                          : "use the custom cabin DSP for interior/exterior acoustics");
        }
    }

    log::info("[radio-acoustics] ===== done =====");
}

} // namespace fh6r::fmod
