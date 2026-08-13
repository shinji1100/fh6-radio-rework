#include "fh6r/cabin_dsp.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kPi = 3.14159265358979323846f;

bool finite(float x) { return std::isfinite(x); }

// Feed a sine tone and report the peak output magnitude (for bound checks).
float run_tone(fh6r::CabinDSP& dsp, fh6r::CabinMode mode, int samples) {
    dsp.set_mode(mode);
    dsp.reset();
    float peak = 0.0f;
    for (int i = 0; i < samples; ++i) {
        float l = 0.5f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
        float r = l;
        dsp.process(l, r);
        assert(finite(l) && finite(r));
        peak = std::fmax(peak, std::fmax(std::fabs(l), std::fabs(r)));
    }
    return peak;
}
} // namespace

int main() {
    fh6r::CabinDSP dsp;

    // 1. Silence in -> silence out (no DC drift / self-oscillation).
    {
        dsp.set_mode(fh6r::CabinMode::Cockpit);
        dsp.reset();
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < 48000; ++i) dsp.process(l, r);
        assert(l == 0.0f && r == 0.0f);
    }

    // 2. Cockpit tone stays bounded (limiter keeps it <= 1.0 + epsilon).
    {
        const float peak = run_tone(dsp, fh6r::CabinMode::Cockpit, 48000);
        assert(peak <= 1.001f);
        assert(peak > 0.0f);
    }

    // 3. Exterior peak differs from Cockpit peak (distinct, quieter reverb tail).
    {
        const float cp = run_tone(dsp, fh6r::CabinMode::Cockpit, 48000);
        const float ep = run_tone(dsp, fh6r::CabinMode::Exterior, 48000);
        assert(std::fabs(cp - ep) > 1e-4f);
    }

    // 4. Unknown mode produces a finite, bounded signal (fail-safe default).
    {
        const float up = run_tone(dsp, fh6r::CabinMode::Unknown, 48000);
        assert(up <= 1.001f);
    }

    // 5. Disabling EQ keeps the signal bounded and finite (no unstable stage).
    {
        dsp.set_eq_enabled(false);
        const float p = run_tone(dsp, fh6r::CabinMode::Cockpit, 24000);
        assert(p <= 1.001f);
        dsp.set_eq_enabled(true);
    }

    // 6. Extreme reflection amount cannot blow up (clamped + softclip).
    {
        dsp.set_reflection_amount(1.0f);
        dsp.set_rear_fill(1.0f);
        dsp.set_crossfeed(0.6f);
        const float p = run_tone(dsp, fh6r::CabinMode::Cockpit, 24000);
        assert(p <= 1.001f);
        dsp.set_reflection_amount(0.08f);
        dsp.set_rear_fill(0.2f);
        dsp.set_crossfeed(0.15f);
    }

    // 7. Stereo independence: a hard-panned right input still yields a finite
    //    left output (crossfeed works, does not NaN).
    {
        dsp.set_mode(fh6r::CabinMode::Cockpit);
        dsp.reset();
        for (int i = 0; i < 4800; ++i) {
            float l = 0.0f, r = 0.9f;
            dsp.process(l, r);
            assert(finite(l) && finite(r));
        }
    }

    std::puts("cabin_dsp_test: all checks passed");
    return 0;
}
