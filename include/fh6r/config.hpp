#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace fh6r {
struct ConfigSnapshot {
    std::string endpoint_id;
    float gain = 1.0f;
    bool native_stereo = false;
    bool spatial_audio = true;
    bool binaural = false;
    std::string cabin_profile = "sportscar";
    std::string speaker_layout = "premium6";
    float cabin_wet = 0.18f;
    float driver_offset = 0.35f;
    float head_width_m = 0.18f;
    float cabin_openness = 0.0f;
    std::uint16_t dashboard_port = 8420;
};

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path path);
    ConfigSnapshot load();
    ConfigSnapshot snapshot() const;
    void set_endpoint(std::string endpoint);
    void set_gain(float gain);
    void set_native_stereo(bool enabled);
    bool save() const;
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
    mutable std::mutex mu_;
    ConfigSnapshot cfg_;
};
} // namespace fh6r
