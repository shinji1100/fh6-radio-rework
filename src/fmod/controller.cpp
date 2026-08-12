#include "fh6r/fmod/controller.hpp"
#include "fh6r/fmod/radio_discovery.hpp"
#include "fh6r/log.hpp"
#include <chrono>
#include <string_view>

namespace fh6r::fmod {
namespace {
using namespace std::chrono_literals;
constexpr std::string_view kCarrier = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
}

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
        s.sound_name = sound_name_;
    }
    s.dsp_attached = bridge_.stats().attached;
    return s;
}

bool Controller::discover_target() noexcept {
    const auto disc = discover_radio_instances(image_);
    const RadioInstance* exact = nullptr;
    for (const auto& instance : disc.instances) {
        if (instance.sound_name != kCarrier) continue;
        if (bridge_.channel_alive(instance.radio_stream)) {
            exact = &instance;
            break;
        }
        if (!exact) exact = &instance;
    }

    if (!exact) {
        std::scoped_lock lk{mu_};
        target_found_ = false;
        sound_name_.clear();
        return false;
    }

    void* system = resolve_fmod_system(image_, exact->radio_stream);
    if (!system) return false;
    bridge_.set_target(*exact, system);
    bridge_.retarget_if_needed();
    {
        std::scoped_lock lk{mu_};
        target_found_ = true;
        sound_name_ = exact->sound_name;
    }
    return true;
}

void Controller::run(std::stop_token stop) noexcept {
    log::info("[controller] waiting for the Streamer Mode carrier");
    auto next_discovery = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        bridge_.retarget_if_needed();

        // Re-discover periodically. We intentionally never fall back to a random
        // radio stream: attaching to the wrong instance would overwrite a native
        // station. If the carrier is absent, the dashboard tells the user to cycle
        // the in-game radio instead.
        if (now >= next_discovery) {
            discover_target();
            next_discovery = now + (bridge_.stats().attached ? 2s : 1s);
        }
        std::this_thread::sleep_for(100ms);
    }
    log::info("[controller] stopped");
}
} // namespace fh6r::fmod
