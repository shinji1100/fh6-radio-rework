#include "fh6r/cabin_dsp.hpp"
#include <algorithm>
#include <cmath>

namespace fh6r {
namespace {

// Early-reflection taps at 48 kHz: (delay in samples, gain). Gains already
// include a HF rolloff so the reflection reads as a soft, directionally-located
// "cabin" bloom rather than a discernible echo. Delays span 2.9 - 13.2 ms.
struct ReflectTap { float delay_samples; float gain; };
constexpr ReflectTap kReflect[CabinDSP::kNumReflections] = {
    {3.1f * 48.0f,  0.10f},   // windscreen / dash
    {5.7f * 48.0f,  0.07f},   // side glass
    {9.4f * 48.0f,  0.04f},   // seats / rear
    {13.2f * 48.0f, 0.025f},  // rear glass (soft)
};

// Soft limiter: transparent below `c`, tanh knee above. Bounded to <= 1.0.
float softclip(float x, float c) noexcept {
    if (c >= 1.0f) return std::clamp(x, -1.0f, 1.0f);
    if (x > c) {
        const float d = (x - c) / (1.0f - c);
        return c + (1.0f - c) * std::tanh(d);
    }
    if (x < -c) {
        const float d = (-x - c) / (1.0f - c);
        return -c - (1.0f - c) * std::tanh(d);
    }
    return x;
}

} // namespace

CabinDSP::CabinDSP() {
    // Compute the cabin EQ once at 48 kHz (RBJ cookbook). This runs on the
    // control thread, not in the mixer callback.
    constexpr double pi = 3.14159265358979323846;
    constexpr double fs = 48000.0;

    // Stage definitions: {type, f0, gain_db, Q}.
    // 0: low shelf  80 Hz +1.5 dB
    // 1: peaking   250 Hz -1.0 dB Q=1
    // 2: peaking  3000 Hz -0.5 dB Q=1
    // 3: high shelf 9 kHz -2.0 dB
    struct Spec { int type; double f0; double db; double q; };
    constexpr Spec specs[kNumEqStages] = {
        {0, 80.0,   1.5,  0.707},
        {1, 250.0, -1.0,  1.0},
        {1, 3000.0, -0.5, 1.0},
        {2, 9000.0, -2.0, 0.707},
    };

    for (int s = 0; s < kNumEqStages; ++s) {
        const double w0 = 2.0 * pi * specs[s].f0 / fs;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double A = std::pow(10.0, specs[s].db / 40.0);
        double a0 = 1.0, a1 = 0.0, a2 = 0.0, b0 = 0.0, b1 = 0.0, b2 = 0.0;

        if (specs[s].type == 1) { // peaking
            const double alpha = sw / (2.0 * specs[s].q);
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cw;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha / A;
        } else if (specs[s].type == 0) { // low shelf
            const double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 1.0 - 1.0) + 2.0);
            const double sqA = std::sqrt(A);
            b0 = A * ((A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha);
            a0 = (A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            a2 = (A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha;
        } else { // high shelf
            const double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 1.0 - 1.0) + 2.0);
            const double sqA = std::sqrt(A);
            b0 = A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha);
            a0 = (A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            a2 = (A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha;
        }

        eq_[s].b0 = static_cast<float>(b0 / a0);
        eq_[s].b1 = static_cast<float>(b1 / a0);
        eq_[s].b2 = static_cast<float>(b2 / a0);
        eq_[s].a1 = static_cast<float>(a1 / a0);
        eq_[s].a2 = static_cast<float>(a2 / a0);
    }
}

void CabinDSP::set_crossfeed(float v) noexcept {
    if (!std::isfinite(v)) return;
    crossfeed_.store(std::clamp(v, 0.0f, 0.6f), std::memory_order_release);
}
void CabinDSP::set_driver_offset(float v) noexcept {
    if (!std::isfinite(v)) return;
    driver_offset_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_release);
}
void CabinDSP::set_rear_fill(float v) noexcept {
    if (!std::isfinite(v)) return;
    rear_fill_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_release);
}
void CabinDSP::set_rear_delay_ms(float v) noexcept {
    if (!std::isfinite(v)) return;
    rear_delay_ms_.store(std::clamp(v, 0.0f, 4.0f), std::memory_order_release);
}
void CabinDSP::set_reflection_amount(float v) noexcept {
    if (!std::isfinite(v)) return;
    reflection_amount_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_release);
}
void CabinDSP::set_eq_enabled(bool on) noexcept {
    eq_enabled_.store(on, std::memory_order_release);
}
void CabinDSP::set_ceiling(float v) noexcept {
    if (!std::isfinite(v)) return;
    ceiling_.store(std::clamp(v, 0.5f, 1.0f), std::memory_order_release);
}

void CabinDSP::reset() noexcept {
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < kDelayLen; ++i) dl_[ch][i] = 0.0f;
    for (int s = 0; s < kNumEqStages; ++s)
        for (int ch = 0; ch < 2; ++ch) { bq_z1_[s][ch] = 0.0f; bq_z2_[s][ch] = 0.0f; }
    dpos_ = 0;
}

float CabinDSP::delay_read(int ch, float d) const noexcept {
    // d >= 1 (all delays > 0). Linear-interpolated fractional delay.
    const float idx = static_cast<float>(dpos_) - d;
    const int i0 = static_cast<int>(std::floor(idx));
    const float frac = idx - static_cast<float>(i0);
    const int i1 = i0 - 1;
    const float s0 = dl_[ch][i0 & kDelayMask];
    const float s1 = dl_[ch][i1 & kDelayMask];
    return s0 + frac * (s1 - s0);
}

float CabinDSP::run_eq(int ch, float x) noexcept {
    // Cascaded transposed direct-form II.
    for (int s = 0; s < kNumEqStages; ++s) {
        const Biquad& c = eq_[s];
        const float y = c.b0 * x + bq_z1_[s][ch];
        bq_z1_[s][ch] = c.b1 * x - c.a1 * y + bq_z2_[s][ch];
        bq_z2_[s][ch] = c.b2 * x - c.a2 * y;
        x = y;
    }
    return x;
}

void CabinDSP::process(float& l, float& r) noexcept {
    const CabinMode m = mode_.load(std::memory_order_relaxed);

    float xf = crossfeed_.load(std::memory_order_relaxed);
    const float drv = driver_offset_.load(std::memory_order_relaxed);
    float rear = rear_fill_.load(std::memory_order_relaxed);
    const float rear_delay = rear_delay_ms_.load(std::memory_order_relaxed) * 48.0f;
    float refl = reflection_amount_.load(std::memory_order_relaxed);
    const bool eq = eq_enabled_.load(std::memory_order_relaxed);
    const float ceilv = ceiling_.load(std::memory_order_relaxed);

    // Mode shaping: exterior keeps the signal dry and narrow; unknown is a
    // gentle default. Cockpit uses the full set of cabin cues.
    if (m == CabinMode::Exterior) {
        xf *= 0.35f; rear *= 0.25f; refl *= 0.15f;
    } else if (m == CabinMode::Unknown) {
        rear *= 0.5f; refl *= 0.3f;
    }

    // Guard against pathological input (normal path is well inside [-2,2]).
    l = std::clamp(l, -4.0f, 4.0f);
    r = std::clamp(r, -4.0f, 4.0f);

    // 1. Stereo matrix: crossfeed.
    const float ml = l + xf * r;
    const float mr = r + xf * l;

    // Driver offset (LHD: driver on the left). drv in 0..1 biases the balance
    // toward the driver side; bounded to avoid a runaway gain.
    const float lg = 1.0f + drv * 0.15f;
    const float rg = 1.0f - drv * 0.15f;

    // Write the matrix signal into the delay lines (source for rear + reflections).
    dl_[0][dpos_] = ml * lg;
    dl_[1][dpos_] = mr * rg;

    // 2. Rear fill via fractional delay (crossfed into the opposite side a bit).
    const float rl = delay_read(0, rear_delay);
    const float rr = delay_read(1, rear_delay);
    float out_l = ml * lg + rear * (0.85f * rl + 0.15f * rr);
    float out_r = mr * rg + rear * (0.85f * rr + 0.15f * rl);

    // 3. Early reflections.
    float ref_l = 0.0f, ref_r = 0.0f;
    for (int i = 0; i < kNumReflections; ++i) {
        ref_l += delay_read(0, kReflect[i].delay_samples) * kReflect[i].gain;
        ref_r += delay_read(1, kReflect[i].delay_samples) * kReflect[i].gain;
    }
    out_l += refl * ref_l;
    out_r += refl * ref_r;

    // Advance the write position only after all reads have completed.
    dpos_ = (dpos_ + 1) & kDelayMask;

    // 4. Cabin EQ.
    if (eq) {
        out_l = run_eq(0, out_l);
        out_r = run_eq(1, out_r);
    }

    // 5. Soft limiter.
    l = softclip(out_l, ceilv);
    r = softclip(out_r, ceilv);
}

} // namespace fh6r
