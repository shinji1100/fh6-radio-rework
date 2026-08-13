#pragma once
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/fmod/metadata_injector.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include "fh6r/media_session.hpp"
#include <mutex>
#include <string>
#include <thread>

namespace fh6r::fmod {
struct ControllerStats {
    bool target_found = false;
    bool dsp_attached = false;
    bool streamer_mode = false;
    std::string station_name;
    std::string sound_name;
};

class Controller {
public:
    Controller(DSPBridge& bridge, const PEImage& image);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    ControllerStats stats() const;
private:
    void run(std::stop_token stop) noexcept;
    bool discover_target() noexcept;
    bool refresh_station_gate() noexcept;
    void clear_target_state() noexcept;
    void update_metadata() noexcept;

    DSPBridge& bridge_;
    const PEImage& image_;
    MetadataInjector metadata_;
    MediaSessionProvider media_session_;
    mutable std::mutex mu_;
    bool target_found_ = false;
    bool streamer_mode_ = false;
    bool event_probed_ = false;
    std::string station_name_;
    std::string sound_name_;
    void** radio_state_slot_ = nullptr;
    int radio_state_retry_ticks_ = 0;
    std::jthread thread_;
};
} // namespace fh6r::fmod
