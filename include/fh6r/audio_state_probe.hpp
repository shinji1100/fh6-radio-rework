#pragma once
#include "fh6r/fmod/dsp_bridge.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh6r {

// One sampled FMOD listener. valid=false when the listener attributes could
// not be read (API missing, ABI mismatch, or no active listener).
struct ListenerSnap {
    bool valid = false;
    fmod::FMOD_VEC pos{}, vel{}, fwd{}, up{};
};

// One DSP on the radio channel. params[] holds up to kMaxParams float values
// for the first N parameters; num_params is the count FMOD reported.
struct DSPSnap {
    bool valid = false;
    std::int32_t type = 0;
    std::int32_t num_params = 0;
    float params[16]{};
};

struct AudioStateSnapshot {
    bool attached = false;
    std::uint32_t channel_handle = 0;
    ListenerSnap listener;
    std::vector<DSPSnap> dsps;
    std::int64_t taken_ms = 0;
};

// Phase 1.1 audio-state probe. Owns a background thread that samples the live
// radio channel (its DSP chain and the FMOD 3D listener) roughly once per
// second while a target is attached. Read-only with respect to FMOD; every
// call is wrapped in seh_call so a bad ABI guess or a dead channel cannot take
// the game down. The latest snapshot is exposed to the dashboard/diff layer.
class AudioStateProbe {
public:
    explicit AudioStateProbe(fmod::DSPBridge& bridge, std::string data_dir);
    ~AudioStateProbe();
    AudioStateProbe(const AudioStateProbe&) = delete;
    AudioStateProbe& operator=(const AudioStateProbe&) = delete;

    AudioStateSnapshot snapshot() const;

private:
    void run(std::stop_token stop) noexcept;
    void sample() noexcept;

    fmod::DSPBridge& bridge_;
    std::string data_dir_;
    bool reverb_scouted_ = false;
    int reverb_attempts_ = 0;
    mutable std::mutex mu_;
    AudioStateSnapshot snap_;
    std::jthread thread_;
};

} // namespace fh6r
