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
#include <windows.h>

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

constexpr WORD kXInputGamepadRightShoulder = 0x0200;

struct XInputGamepad {
    WORD buttons;
    BYTE left_trigger;
    BYTE right_trigger;
    SHORT thumb_lx;
    SHORT thumb_ly;
    SHORT thumb_rx;
    SHORT thumb_ry;
};

struct XInputState { DWORD packet_number; XInputGamepad gamepad; };
using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XInputState*);

XInputGetStateFn resolve_xinput_get_state() noexcept {
    static const auto fn = []() noexcept -> XInputGetStateFn {
        constexpr const wchar_t* names[]{L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const auto* name : names) {
            HMODULE module = GetModuleHandleW(name);
            if (!module) module = LoadLibraryW(name);
            if (module) {
                if (const auto proc = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<XInputGetStateFn>(proc);
            }
        }
        return nullptr;
    }();
    return fn;
}

bool game_has_focus() noexcept {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool camera_button_down() noexcept {
    if (!game_has_focus()) return false;
    if ((GetAsyncKeyState(VK_TAB) & 0x8000) != 0) return true;
    if (const auto get_state = resolve_xinput_get_state()) {
        for (DWORD user = 0; user < 4; ++user) {
            XInputState state{};
            if (get_state(user, &state) == ERROR_SUCCESS &&
                (state.gamepad.buttons & kXInputGamepadRightShoulder) != 0)
                return true;
        }
    }
    return false;
}

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
    : bridge_{bridge}, image_{image},
      camera_thread_{[this](std::stop_token s) { camera_run(s); }},
      thread_{[this](std::stop_token s) { run(s); }} {}
Controller::~Controller() {
    camera_thread_.request_stop();
    thread_.request_stop();
    if (camera_thread_.joinable()) camera_thread_.join();
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
    s.camera_view = camera_view_.load(std::memory_order_acquire);
    s.camera_anchored = camera_tracker_.anchored();
    s.camera_labeled_clusters = camera_tracker_.labeled_clusters();
    s.camera_clusters = camera_tracker_.cluster_count();
    s.camera_events = camera_tracker_.events();
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

void Controller::apply_camera_view(CabinMode mode, const char* cause) noexcept {
    camera_view_.store(mode, std::memory_order_release);
    bridge_.set_cabin_mode(mode);
    log::info("[camera] {} -> {}", cause, cabin_mode_name(mode));
}

void Controller::camera_run(std::stop_token stop) noexcept {
    camera_tracker_.reset(CabinMode::Dashboard);
    while (!stop.stop_requested()) {
        const bool down = camera_button_down();
        const bool edge = down && !camera_button_down_;
        camera_button_down_ = down;

        // Ground truth: the game's own FMOD listener pose, resolved at
        // runtime through pattern-scanned API entry points. Invalid when the
        // stream is not attached; the tracker then degrades to edge counting.
        CameraGeometry geo;
        const auto& fns = bridge_.fns();
        void* sys = (bridge_.channel_handle() != 0) ? bridge_.system() : nullptr;
        if (sys && fns.get3d_listener_attrs) {
            FMOD_VEC pos, vel, fwd, up;
            std::uint32_t rc = ~0u;
            if (seh_call([&] { rc = fns.get3d_listener_attrs(sys, 0, &pos, &vel, &fwd, &up); }) && rc == 0) {
                geo.pos[0] = pos.x; geo.pos[1] = pos.y; geo.pos[2] = pos.z;
                geo.fwd[0] = fwd.x; geo.fwd[1] = fwd.y; geo.fwd[2] = fwd.z;
                geo.up[0] = up.x; geo.up[1] = up.y; geo.up[2] = up.z;
                geo.valid = true;
            }
        }

        const auto r = camera_tracker_.update(geo, 0.01f, edge);
        if (r.switched) {
            const char* cause = r.snapped ? "geometry anchor"
                : (r.source == CameraTracker::EventSource::GeometryJump ? "geometry jump" : "input edge");
            apply_camera_view(camera_tracker_.view(), cause);
        }
        std::this_thread::sleep_for(10ms);
    }
}

void Controller::sync_camera_view(CabinMode mode) noexcept {
    if (mode == CabinMode::Unknown) return;
    camera_tracker_.set_view(mode);
    apply_camera_view(mode, "manual sync");
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
    const auto channel = bridge_.channel_handle();
    if (channel != 0 && channel != camera_channel_handle_) {
        camera_channel_handle_ = channel;
        // Vehicle swap (loading blackout): drop learned clusters and re-anchor.
        camera_tracker_.reset(CabinMode::Dashboard);
        apply_camera_view(CabinMode::Dashboard, "reset");
    }
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
