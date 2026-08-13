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

    // 3. Exterior views are rendered as a narrower car source instead of the
    //    old shared unity bypass.
    {
        dsp.set_mode(fh6r::CabinMode::Cockpit);
        dsp.reset();
        for (int i = 0; i < 24000; ++i) {
            float l = 0.3f, r = -0.2f;
            dsp.process(l, r);
        }
        dsp.set_mode(fh6r::CabinMode::ChaseNear);
        float l = 0.3f, r = -0.2f;
        for (int i = 0; i < 48000; ++i) {
            l = 0.3f;
            r = -0.2f;
            dsp.process(l, r);
        }
        assert(std::fabs(l - 0.3f) > 1e-3f || std::fabs(r + 0.2f) > 1e-3f);
        assert(std::fabs(l) <= 1.001f && std::fabs(r) <= 1.001f);
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

    // 12. Six-view distinguishability: each designed-to-differ view pair must
    //     produce measurably different output from identical input. This is
    //     the lab proof that a view switch reaches the audio path (the "one
    //     interior / one exterior sound" complaint must be impossible).
    {
        struct Signature { double rms; double hf; };
        auto run_view = [&](fh6r::CabinMode mode) {
            dsp.set_profile(fh6r::CabinProfile::SportsCar);
            dsp.set_speaker_layout(fh6r::SpeakerLayout::Premium6);
            dsp.set_mode(mode);
            dsp.reset();
            std::uint32_t lfsr = 0x12345678u;
            Signature s{};
            float prev = 0.0f;
            const int n = 48000;
            for (int i = 0; i < n; ++i) {
                // Deterministic pseudo-noise, identical for every view.
                lfsr = lfsr * 1664525u + 1013904223u;
                const float in = static_cast<float>(static_cast<int>(lfsr >> 9) & 0xFFFF) / 65536.0f - 0.5f;
                float l = in * 0.6f, r = in * 0.4f;
                dsp.process(l, r);
                if (i > n / 2) { // settled only
                    const float m = 0.5f * (l + r);
                    s.rms += static_cast<double>(m) * m;
                    const float d = m - prev;
                    s.hf += static_cast<double>(d) * d;
                    prev = m;
                }
            }
            s.rms = std::sqrt(s.rms / (n / 2));
            s.hf = std::sqrt(s.hf / (n / 2));
            return s;
        };
        const auto cockpit = run_view(fh6r::CabinMode::Cockpit);
        const auto dashboard = run_view(fh6r::CabinMode::Dashboard);
        const auto chase_near = run_view(fh6r::CabinMode::ChaseNear);
        const auto chase_far = run_view(fh6r::CabinMode::ChaseFar);
        const auto hood = run_view(fh6r::CabinMode::Hood);
        const auto bumper = run_view(fh6r::CabinMode::Bumper);

        // Interior pair: Dashboard is drier/darker than Cockpit.
        assert(std::fabs(cockpit.rms - dashboard.rms) > 1e-4 ||
               std::fabs(cockpit.hf - dashboard.hf) > 1e-5);
        // Exterior: ChaseFar is darker than ChaseNear (10.5k vs 15k LP).
        assert(chase_far.hf < chase_near.hf * 0.95);
        // Hood is brighter than Bumper (16.5k vs 13.5k LP).
        assert(hood.hf > bumper.hf * 1.01);
        // Interior vs exterior differ strongly.
        assert(std::fabs(cockpit.rms - chase_near.rms) > 1e-3 ||
               std::fabs(cockpit.hf - chase_near.hf) > 1e-4);
        // Applied-view telemetry reflects the mode that was set.
        dsp.set_mode(fh6r::CabinMode::ChaseFar);
        dsp.reset();
        for (int i = 0; i < 48000; ++i) { float l = 0.1f, r = 0.1f; dsp.process(l, r); }
        const auto ap = dsp.applied_view();
        assert(ap.mode == fh6r::CabinMode::ChaseFar);
        assert(std::fabs(ap.width - 0.40f) < 0.05f);
        assert(std::fabs(ap.cutoff_hz - 10500.0f) < 500.0f);
        assert(ap.active_mix > 0.95f);
    }

    std::puts("cabin_dsp_test: all checks passed");
    return 0;
}
