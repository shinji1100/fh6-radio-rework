#pragma once
#include "fh6r/camera_tracker.hpp"
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/fmod/metadata_injector.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include "fh6r/media_session.hpp"
#include <atomic>
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
    CabinMode camera_view = CabinMode::Unknown;
    bool camera_anchored = false;
    int camera_labeled_clusters = 0;
    int camera_clusters = 0;
    std::uint64_t camera_events = 0;
};

class Controller {
public:
    Controller(DSPBridge& bridge, const PEImage& image);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    ControllerStats stats() const;
    // Manual resync (debug/fallback only; the tracker self-corrects in normal
    // operation). Also teaches the tracker the current cluster's label.
    void sync_camera_view(CabinMode mode) noexcept;
private:
    void run(std::stop_token stop) noexcept;
    bool discover_target() noexcept;
    bool refresh_station_gate() noexcept;
    void camera_run(std::stop_token stop) noexcept;
    void apply_camera_view(CabinMode mode, const char* cause) noexcept;
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
    bool radio_acoustics_probed_ = false;
    bool studio_identity_checked_ = false;
    std::string station_name_;
    std::string sound_name_;
    void** radio_state_slot_ = nullptr;
    int radio_state_retry_ticks_ = 0;
    CameraTracker camera_tracker_;
    std::atomic<CabinMode> camera_view_{CabinMode::Dashboard};
    bool camera_button_down_ = false;
    std::uint32_t camera_channel_handle_ = 0;
    std::jthread camera_thread_;
    std::jthread thread_;
};
} // namespace fh6r::fmod
