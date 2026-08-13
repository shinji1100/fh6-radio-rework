#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace fh6r {

// FH6's fixed driving-camera cycle.  Keeping the concrete view in the DSP
// avoids collapsing every non-interior camera to one generic bypass.
enum class CabinMode : std::uint8_t {
    Unknown = 0,
    Cockpit,
    Dashboard,
    ChaseNear,
    ChaseFar,
    Hood,
    Bumper
};

const char* cabin_mode_name(CabinMode mode) noexcept;
CabinMode cabin_mode_from_name(std::string_view name) noexcept;

// The five profiles currently enabled by FH6's IRMappings.xml plus a neutral
// fallback. Profiles are deliberately acoustic classes rather than car names.
enum class CabinProfile : std::uint8_t {
    Generic = 0,
    Luxury,
    Race,
    Saloon,
    SportsCar,
    Van,
    Count
};

enum class SpeakerLayout : std::uint8_t {
    Front2 = 0,
    Cabin4,
    Premium6,
    Surround8
};

// Real-time, allocation-free virtual car audio renderer at 48 kHz.
//
// Signal path:
//   stereo source -> parameterized virtual speakers
//                 -> stereo or analytic binaural renderer
//                 -> early cabin reflections + 4-line damped FDN
//                 -> profile EQ -> limiter
//
// Vehicle changes are intentionally direct: call reset(), select the new
// profile/layout, then resume audio after FH6's loading blackout. Convertible
// roof changes use set_openness(), which is smoothed in the audio callback.
class CabinDSP {
public:
    static constexpr int kNumEqStages = 4;
    static constexpr int kMaxSpeakers = 8;
    static constexpr int kNumFdnLines = 4;
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    CabinDSP();
    ~CabinDSP() = default;
    CabinDSP(const CabinDSP&) = delete;
    CabinDSP& operator=(const CabinDSP&) = delete;

    void set_mode(CabinMode m) noexcept { mode_.store(m, std::memory_order_release); }
    CabinMode mode() const noexcept { return mode_.load(std::memory_order_acquire); }

    void set_profile(CabinProfile p) noexcept;
    CabinProfile profile() const noexcept { return profile_.load(std::memory_order_acquire); }
    bool set_profile_name(std::string_view name) noexcept;

    void set_speaker_layout(SpeakerLayout layout) noexcept;
    SpeakerLayout speaker_layout() const noexcept { return layout_.load(std::memory_order_acquire); }
    bool set_speaker_layout_name(std::string_view name) noexcept;

    void set_binaural(bool on) noexcept { binaural_.store(on, std::memory_order_release); }
    bool binaural() const noexcept { return binaural_.load(std::memory_order_acquire); }

    // Existing controls remain available for the dashboard/control thread.
    void set_crossfeed(float v) noexcept;         // 0..0.6
    void set_driver_offset(float v) noexcept;     // 0..1, LHD listener bias
    void set_rear_fill(float v) noexcept;         // 0..1
    void set_rear_delay_ms(float v) noexcept;     // 0..4 ms
    void set_reflection_amount(float v) noexcept; // 0..1
    void set_cabin_wet(float v) noexcept;         // 0..1
    void set_head_width_m(float v) noexcept;      // 0.12..0.24 m
    void set_openness(float v) noexcept;          // 0 roof closed, 1 fully open
    void set_eq_enabled(bool on) noexcept;
    void set_ceiling(float v) noexcept;

    void process(float& left, float& right) noexcept;
    void reset() noexcept;

private:
    static constexpr int kSourceDelayLen = 2048; // 42.7 ms
    static constexpr int kSourceDelayMask = kSourceDelayLen - 1;
    static constexpr int kFdnBufferLen = 4096;   // 85.3 ms per line

    float source_read(int channel, float delay_samples) const noexcept;
    float run_eq(int profile_index, int channel, float x) noexcept;
    float run_head_shadow(int speaker, int ear, float x, float alpha) noexcept;
    void build_eq_banks() noexcept;
    void build_fdn_feedback() noexcept;

    std::atomic<CabinMode> mode_{CabinMode::Unknown};
    std::atomic<CabinProfile> profile_{CabinProfile::SportsCar};
    std::atomic<SpeakerLayout> layout_{SpeakerLayout::Premium6};
    std::atomic<bool> binaural_{false};
    std::atomic<float> crossfeed_{0.08f};
    std::atomic<float> driver_offset_{0.35f};
    std::atomic<float> rear_fill_{0.45f};
    std::atomic<float> rear_delay_ms_{1.2f};
    std::atomic<float> reflection_amount_{0.65f};
    std::atomic<float> cabin_wet_{0.18f};
    std::atomic<float> head_width_m_{0.18f};
    std::atomic<float> openness_{0.0f};
    std::atomic<bool> eq_enabled_{true};
    std::atomic<float> ceiling_{0.98f};

    float source_delay_[2][kSourceDelayLen]{};
    int source_pos_ = 0;

    float hrtf_lp_[kMaxSpeakers][2]{};

    float fdn_[kNumFdnLines][kFdnBufferLen]{};
    int fdn_pos_[kNumFdnLines]{};
    float fdn_lp_[kNumFdnLines]{};
    float fdn_feedback_[static_cast<int>(CabinProfile::Count)][kNumFdnLines]{};

    float bq_z1_[kNumEqStages][2]{};
    float bq_z2_[kNumEqStages][2]{};
    Biquad eq_[static_cast<int>(CabinProfile::Count)][kNumEqStages]{};

    float openness_state_ = 0.0f;
    float mode_mix_state_ = 0.0f;
    float view_gain_state_ = 1.0f;
    float view_width_state_ = 1.0f;
    float view_cutoff_state_ = 18000.0f;
    float exterior_lp_[2]{};
};

const char* cabin_profile_name(CabinProfile profile) noexcept;
const char* speaker_layout_name(SpeakerLayout layout) noexcept;

} // namespace fh6r
