#pragma once
#include <atomic>
#include <cstdint>

namespace fh6r {

// Acoustic state of the game listener, as far as the cabin engine needs it.
// Kept deliberately coarse: v1 only distinguishes Unknown / Cockpit / Exterior.
// The real signal source (memory diff / Studio parameter) is wired elsewhere;
// this engine just consumes the resulting enum.
enum class CabinMode : std::uint8_t { Unknown = 0, Cockpit = 1, Exterior = 2 };

// Cabin-simulation DSP. Fixed sample rate of 48 kHz (matches the audio path).
//
// Real-time contract (must hold inside the FMOD mixer callback):
//   - no allocation, no locking, no IO
//   - fixed-size state only (delay lines + biquad memories)
//   - parameters are read via relaxed atomics; a torn value for one frame is
//     harmless (all values are bounded, can never produce NaN / clip)
//
// Processing order (per stereo sample pair, in place):
//   stereo matrix (crossfeed + driver offset)
//     -> rear-fill fractional delay
//     -> early reflections (4 fixed taps)
//     -> cabin EQ (4 biquads, coefficients computed once at 48 kHz)
//     -> soft limiter
class CabinDSP {
public:
    static constexpr int kNumReflections = 4;
    static constexpr int kNumEqStages = 4;

    CabinDSP();
    ~CabinDSP() = default;
    CabinDSP(const CabinDSP&) = delete;
    CabinDSP& operator=(const CabinDSP&) = delete;

    void set_mode(CabinMode m) noexcept { mode_.store(m, std::memory_order_release); }
    CabinMode mode() const noexcept { return mode_.load(std::memory_order_acquire); }

    // Control-thread setters (clamped to safe ranges).
    void set_crossfeed(float v) noexcept;        // 0..0.6
    void set_driver_offset(float v) noexcept;    // 0..1 (balance toward driver side)
    void set_rear_fill(float v) noexcept;        // 0..1 rear speaker level
    void set_rear_delay_ms(float v) noexcept;    // 0..4 ms
    void set_reflection_amount(float v) noexcept;// 0..1 early-reflection wet
    void set_eq_enabled(bool on) noexcept;
    void set_ceiling(float v) noexcept;          // limiter ceiling 0.5..1.0

    // Process one stereo sample pair in place. Input/output in [-1,1].
    void process(float& l, float& r) noexcept;

    // Reset delay lines and biquad memories (call on device change / retarget).
    void reset() noexcept;

private:
    static constexpr int kDelayLen = 1024;   // ~21.3 ms @ 48 kHz
    static constexpr int kDelayMask = kDelayLen - 1;

    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    float delay_read(int ch, float delay_samples) const noexcept;
    float run_eq(int ch, float x) noexcept;

    std::atomic<CabinMode> mode_{CabinMode::Unknown};
    std::atomic<float> crossfeed_{0.15f};
    std::atomic<float> driver_offset_{0.35f};
    std::atomic<float> rear_fill_{0.20f};
    std::atomic<float> rear_delay_ms_{1.2f};
    std::atomic<float> reflection_amount_{0.08f};
    std::atomic<bool> eq_enabled_{true};
    std::atomic<float> ceiling_{0.98f};

    float dl_[2][kDelayLen]{};   // stereo delay lines (matrix signal)
    int dpos_ = 0;               // write index, advances every sample
    float bq_z1_[kNumEqStages][2]{}; // biquad z^-1 memories (2 channels)
    float bq_z2_[kNumEqStages][2]{}; // biquad z^-2 memories
    Biquad eq_[kNumEqStages];    // coefficients computed once at 48 kHz
};

} // namespace fh6r
