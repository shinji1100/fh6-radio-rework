#include "fh6r/fmod/controller.hpp"
#include "fh6r/fmod/event_instance_probe.hpp"
#include "fh6r/fmod/radio_discovery.hpp"
#include "fh6r/fmod/radio_acoustics_probe.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/fmod/studio_system_scout.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace fh6r::fmod {
namespace {
using namespace std::chrono_literals;

constexpr std::string_view kStreamerStation = "Streamer Mode";
constexpr std::string_view kRadioSetStationByName =
    "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 40 "
    "48 8B FA 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 16 "
    "48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB";
constexpr std::ptrdiff_t kRadioStateMovOffset = 18;
constexpr std::ptrdiff_t kStationChain0 = 0x40;
constexpr std::ptrdiff_t kStationChain1 = 0x50;
constexpr std::ptrdiff_t kStationName = 0x200;

// Confirmed on the current FH6 executable by repeated six-view cycling. The
// value is 1 for the two interior views (cockpit and dashboard) and 0 for the
// four exterior views. Build fingerprinting makes this fail closed after an
// update instead of interpreting an unrelated byte at a stale offset.
constexpr std::uint32_t kCameraBuildTimestamp = 0x6A58A086u;
constexpr std::uint32_t kCameraBuildChecksum = 0x0AF21AAAu;
constexpr std::size_t kExpectedDataSize = 0x1CD9DA8u;
constexpr std::size_t kCabinViewFlagOffset = 0x397C0u;

void** resolve_radio_state_slot(const PEImage& image) noexcept {
    auto* hit = find_by_pattern(image, kRadioSetStationByName);
    if (!hit) return nullptr;
    auto* mov = hit + kRadioStateMovOffset;
    std::uint8_t op[3]{};
    if (!is_readable(mov, sizeof(op))) return nullptr;
    std::memcpy(op, mov, sizeof(op));
    if (op[0] != 0x48 || op[1] != 0x8B || op[2] != 0x1D) {
        log::warn("[controller] RadioState signature matched but RIP-load opcode drifted");
        return nullptr;
    }
    std::int32_t disp = 0;
    if (!safe_read(mov + 3, disp)) return nullptr;
    auto* slot = reinterpret_cast<void**>(mov + 7 + disp);
    if (!is_readable(slot, sizeof(void*))) return nullptr;
    log::info("[controller] RadioState global slot=0x{:X}", reinterpret_cast<std::uintptr_t>(slot));
    return slot;
}

bool read_station_name(void** slot, std::string& out) noexcept {
    out.clear();
    if (!slot) return false;
    std::byte* state = nullptr;
    if (!safe_read(slot, state) || !state) return false;
    std::byte* p1 = nullptr;
    if (!safe_read(state + kStationChain0, p1) || !p1) return false;
    std::byte* p2 = nullptr;
    if (!safe_read(p1 + kStationChain1, p2) || !p2) return false;
    auto name = safe_read_msvc_string(p2 + kStationName, 256);
    if (!name || name->empty()) return false;
    out = std::move(*name);
    return true;
}
} // namespace

Controller::Controller(DSPBridge& bridge, const PEImage& image)
    : bridge_{bridge}, image_{image}, thread_{[this](std::stop_token s) { run(s); }} {}
Controller::~Controller() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

ControllerStats Controller::stats() const {
    ControllerStats s;
    {
        std::scoped_lock lk{mu_};
        s.target_found = target_found_;
        s.streamer_mode = streamer_mode_;
        s.station_name = station_name_;
        s.sound_name = sound_name_;
    }
    s.dsp_attached = bridge_.stats().attached;
    return s;
}

void Controller::clear_target_state() noexcept {
    bridge_.clear_target();
    metadata_.set_target(nullptr);
    std::scoped_lock lk{mu_};
    target_found_ = false;
    sound_name_.clear();
}

bool Controller::refresh_station_gate() noexcept {
    if (!radio_state_slot_) {
        if (radio_state_retry_ticks_ > 0) {
            --radio_state_retry_ticks_;
            clear_target_state();
            return false;
        }
        radio_state_slot_ = resolve_radio_state_slot(image_);
        if (!radio_state_slot_) {
            radio_state_retry_ticks_ = 50; // retry about every five seconds
            {
                std::scoped_lock lk{mu_};
                streamer_mode_ = false;
                station_name_.clear();
            }
            clear_target_state();
            return false;
        }
    }

    std::string station;
    if (!read_station_name(radio_state_slot_, station)) {
        {
            std::scoped_lock lk{mu_};
            streamer_mode_ = false;
            station_name_.clear();
        }
        clear_target_state();
        return false;
    }

    const bool streamer = station == kStreamerStation;
    bool changed = false;
    {
        std::scoped_lock lk{mu_};
        changed = station != station_name_;
        station_name_ = station;
        streamer_mode_ = streamer;
    }
    if (changed) log::info("[controller] selected station='{}'", station);
    if (!streamer) {
        clear_target_state();
        return false;
    }
    return true;
}

void Controller::refresh_camera_mode() noexcept {
    if (camera_probe_disabled_) return;
    if (!cabin_view_flag_) {
        if (image_.time_date_stamp != kCameraBuildTimestamp || image_.checksum != kCameraBuildChecksum) {
            log::warn("[camera] unsupported game build timestamp=0x{:08X} checksum=0x{:08X}; cabin auto-gating disabled",
                      image_.time_date_stamp, image_.checksum);
            bridge_.set_cabin_mode(CabinMode::Unknown);
            camera_probe_disabled_ = true;
            return;
        }
        for (const auto& section : image_.sections) {
            const std::string_view name{section.name.data()};
            const auto size = static_cast<std::size_t>(section.end - section.start);
            if (name.starts_with(".data") && size == kExpectedDataSize &&
                kCabinViewFlagOffset + sizeof(std::uint32_t) <= size) {
                cabin_view_flag_ = section.start + kCabinViewFlagOffset;
                break;
            }
        }
        if (!cabin_view_flag_) {
            log::warn("[camera] validated .data section not found; cabin auto-gating disabled");
            bridge_.set_cabin_mode(CabinMode::Unknown);
            camera_probe_disabled_ = true;
            return;
        }
        log::info("[camera] read-only cabin flag=0x{:X} (validated build)",
                  reinterpret_cast<std::uintptr_t>(cabin_view_flag_));
    }

    std::uint32_t raw = 2;
    if (!safe_read(cabin_view_flag_, raw) || raw > 1) {
        if (++invalid_cabin_ticks_ == 10) {
            log::warn("[camera] cabin flag invalid for 1s; bypassing CabinDSP");
            bridge_.set_cabin_mode(CabinMode::Unknown);
            camera_mode_initialized_ = false;
        }
        return;
    }
    invalid_cabin_ticks_ = 0;
    if (raw != pending_cabin_view_) {
        pending_cabin_view_ = raw;
        pending_cabin_ticks_ = 1;
        return;
    }
    if (pending_cabin_ticks_ < 3) ++pending_cabin_ticks_;
    if (pending_cabin_ticks_ < 3) return; // 300 ms debounce rejects transition noise

    const CabinMode mode = raw ? CabinMode::Cockpit : CabinMode::Exterior;
    if (!camera_mode_initialized_ || bridge_.cabin_mode() != mode) {
        bridge_.set_cabin_mode(mode);
        camera_mode_initialized_ = true;
        log::info("[camera] view acoustics -> {}", raw ? "interior" : "exterior/bypass");
    }
}

bool Controller::discover_target() noexcept {
    const auto disc = discover_radio_instances(image_);
    const RadioInstance* active = nullptr;
    void* active_system = nullptr;
    int live_candidates = 0;

    for (const auto& instance : disc.instances) {
        // The current FH6 build exposes five persistent RadioStreamFmod control
        // blocks. The audible one is the candidate with a live FMOD sound and a
        // resolvable channel. Never fall back to an idle/native candidate.
        if (!instance.fmod_sound) continue;
        if (!bridge_.channel_alive(instance.radio_stream)) continue;
        void* system = resolve_fmod_system(image_, instance.radio_stream);
        if (!system) continue;
        ++live_candidates;
        if (!active) {
            active = &instance;
            active_system = system;
        }
    }

    // Crossfades or a future layout change may transiently expose more than one
    // live candidate. Fail closed instead of guessing which native stream to mute.
    if (!active || live_candidates != 1) {
        if (live_candidates > 1)
            log::warn("[controller] {} live RadioStreamFmod candidates; refusing ambiguous attach", live_candidates);
        clear_target_state();
        return false;
    }

    // Heap discovery can take noticeable time. Re-check the station after it so
    // a user switching away from Streamer Mode cannot race an attach.
    if (!refresh_station_gate()) return false;

    bool changed = false;
    {
        std::scoped_lock lk{mu_};
        changed = sound_name_ != active->sound_name || !target_found_;
    }
    // One-shot: verify whether radio_stream is a Studio EventInstance. This is
    // the prerequisite for tracing the radio's downstream routing via
    // EventInstance::getChannelGroup (the Core getChannel enumeration does not
    // expose this channel). Read-only; runs once on first successful attach.
    if (!event_probed_) {
        event_probed_ = true;
        probe_event_instance(image_, active->radio_stream);
    }

    if (!radio_acoustics_probed_) {
        radio_acoustics_probed_ = true;
        probe_radio_acoustics(image_, *active, bridge_.fns());
    }

    if (!studio_identity_checked_) {
        const auto studio = studio_system_snapshot();
        if (studio.valid) {
            studio_identity_checked_ = true;
            if (studio.core_system == active_system) {
                log::info("[studio-system] identity closure: studio_core=0x{:X} radio_core=0x{:X} -> MATCH [PROVEN]",
                          reinterpret_cast<std::uintptr_t>(studio.core_system),
                          reinterpret_cast<std::uintptr_t>(active_system));
            } else {
                log::warn("[studio-system] identity closure: studio_core=0x{:X} radio_core=0x{:X} -> MISMATCH",
                          reinterpret_cast<std::uintptr_t>(studio.core_system),
                          reinterpret_cast<std::uintptr_t>(active_system));
            }
        }
    }

    bridge_.set_target(*active, active_system);
    bridge_.retarget_if_needed();
    metadata_.set_target(active->sample_props_body);
    {
        std::scoped_lock lk{mu_};
        target_found_ = true;
        sound_name_ = active->sound_name;
    }
    if (changed)
        log::info("[controller] active Streamer Mode stream='{}'", active->sound_name);
    return true;
}

void Controller::update_metadata() noexcept {
    MediaSessionProvider::Info info;
    if (media_session_.info(info))
        metadata_.update(info.title, info.artist);
}

void Controller::run(std::stop_token stop) noexcept {
    log::info("[controller] waiting for Streamer Mode and an active radio stream");
    auto next_discovery = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        refresh_camera_mode();

        // Station selection is cheap to poll. It is the safety gate that keeps
        // normal FH6 stations untouched when Streamer Mode is not selected.
        if (!refresh_station_gate()) {
            next_discovery = now;
            std::this_thread::sleep_for(100ms);
            continue;
        }

        bridge_.retarget_if_needed();
        if (now >= next_discovery) {
            discover_target();
            next_discovery = now + (bridge_.stats().attached ? 2s : 1s);
        }
        update_metadata();
        std::this_thread::sleep_for(100ms);
    }
    bridge_.clear_target();
    log::info("[controller] stopped");
}
} // namespace fh6r::fmod
