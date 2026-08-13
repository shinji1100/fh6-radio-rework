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

    // 3. Exterior is a true unity-gain bypass after the short view crossfade.
    {
        dsp.set_mode(fh6r::CabinMode::Cockpit);
        dsp.reset();
        for (int i = 0; i < 24000; ++i) {
            float l = 0.3f, r = -0.2f;
            dsp.process(l, r);
        }
        dsp.set_mode(fh6r::CabinMode::Exterior);
        float l = 0.3f, r = -0.2f;
        for (int i = 0; i < 48000; ++i) {
            l = 0.3f;
            r = -0.2f;
            dsp.process(l, r);
        }
        assert(std::fabs(l - 0.3f) < 1e-4f);
        assert(std::fabs(r + 0.2f) < 1e-4f);
    }

    // 4. Unknown mode fails safe to bypass and remains finite/bounded.
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

    // 8. Every supported vehicle profile and speaker layout is stable.
    {
        constexpr fh6r::CabinProfile profiles[] = {
            fh6r::CabinProfile::Generic, fh6r::CabinProfile::Luxury,
            fh6r::CabinProfile::Race, fh6r::CabinProfile::Saloon,
            fh6r::CabinProfile::SportsCar, fh6r::CabinProfile::Van,
        };
        constexpr fh6r::SpeakerLayout layouts[] = {
            fh6r::SpeakerLayout::Front2, fh6r::SpeakerLayout::Cabin4,
            fh6r::SpeakerLayout::Premium6, fh6r::SpeakerLayout::Surround8,
        };
        for (auto profile : profiles) for (auto layout : layouts) {
            dsp.set_profile(profile);
            dsp.set_speaker_layout(layout);
            const float p = run_tone(dsp, fh6r::CabinMode::Cockpit, 12000);
            assert(p > 0.0f && p <= 1.001f);
        }
    }

    // 9. The binaural renderer creates a cross-ear image from a hard-left
    //    impulse while preserving a left/right level difference.
    {
        dsp.set_speaker_layout(fh6r::SpeakerLayout::Premium6);
        dsp.set_binaural(true);
        dsp.reset();
        double energy_l = 0.0, energy_r = 0.0;
        for (int i = 0; i < 4096; ++i) {
            float l = i == 0 ? 0.8f : 0.0f;
            float r = 0.0f;
            dsp.process(l, r);
            assert(finite(l) && finite(r));
            energy_l += static_cast<double>(l) * l;
            energy_r += static_cast<double>(r) * r;
        }
        assert(energy_l > 0.0 && energy_r > 0.0);
        assert(std::fabs(energy_l - energy_r) > 1e-8);
        dsp.set_binaural(false);
    }

    // 10. A closed-to-open convertible transition is smoothed and remains
    //     bounded without resetting the delay network.
    {
        dsp.set_openness(0.0f);
        dsp.reset();
        for (int i = 0; i < 4096; ++i) {
            float l = 0.2f, r = -0.1f;
            if (i == 512) dsp.set_openness(1.0f);
            dsp.process(l, r);
            assert(finite(l) && finite(r));
            assert(std::fabs(l) <= 1.001f && std::fabs(r) <= 1.001f);
        }
        dsp.set_openness(0.0f);
    }

    // 11. String controls reject unknown values and accept documented names.
    assert(dsp.set_profile_name("luxury"));
    assert(dsp.profile() == fh6r::CabinProfile::Luxury);
    assert(!dsp.set_profile_name("not-a-profile"));
    assert(dsp.set_speaker_layout_name("surround8"));
    assert(dsp.speaker_layout() == fh6r::SpeakerLayout::Surround8);
    assert(!dsp.set_speaker_layout_name("not-a-layout"));

    std::puts("cabin_dsp_test: all checks passed");
    return 0;
}
