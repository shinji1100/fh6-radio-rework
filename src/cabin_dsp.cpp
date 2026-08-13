#include "fh6r/cabin_dsp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace fh6r {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSampleRate = 48000.0f;
constexpr float kSpeedOfSound = 343.0f;

struct Speaker {
    float mix_l;
    float mix_r;
    float sin_azimuth;
    float delay_ms;
    float gain;
    float rear;
};

struct Layout {
    Speaker speakers[CabinDSP::kMaxSpeakers];
    int count;
    float normalization;
};

// The layout is listener-relative. Negative azimuth is left, positive is
// right. Speaker distances become short acoustic delays; rear speakers also
// consume rear_fill and rear_delay_ms.
constexpr Layout kLayouts[] = {
    {{
        {1.00f, 0.05f, -0.47f, 2.4f, 0.78f, 0.0f},
        {0.05f, 1.00f,  0.47f, 3.0f, 0.78f, 0.0f},
    }, 2, 0.90f},
    {{
        {1.00f, 0.06f, -0.50f, 2.3f, 0.72f, 0.0f},
        {0.06f, 1.00f,  0.50f, 3.0f, 0.72f, 0.0f},
        {0.82f, 0.18f, -0.82f, 4.4f, 0.40f, 1.0f},
        {0.18f, 0.82f,  0.82f, 5.0f, 0.40f, 1.0f},
    }, 4, 0.78f},
    {{
        {1.00f, 0.05f, -0.48f, 2.2f, 0.64f, 0.0f},
        {0.05f, 1.00f,  0.48f, 2.9f, 0.64f, 0.0f},
        {0.90f, 0.10f, -0.90f, 3.1f, 0.34f, 0.25f},
        {0.10f, 0.90f,  0.90f, 3.8f, 0.34f, 0.25f},
        {0.76f, 0.24f, -0.70f, 4.8f, 0.28f, 1.0f},
        {0.24f, 0.76f,  0.70f, 5.4f, 0.28f, 1.0f},
    }, 6, 0.72f},
    {{
        {1.00f, 0.04f, -0.46f, 2.2f, 0.58f, 0.0f},
        {0.04f, 1.00f,  0.46f, 2.9f, 0.58f, 0.0f},
        {0.88f, 0.12f, -0.98f, 3.2f, 0.30f, 0.30f},
        {0.12f, 0.88f,  0.98f, 3.9f, 0.30f, 0.30f},
        {0.76f, 0.24f, -0.70f, 4.8f, 0.24f, 1.0f},
        {0.24f, 0.76f,  0.70f, 5.4f, 0.24f, 1.0f},
        {0.50f, 0.50f,  0.00f, 2.6f, 0.18f, 0.0f},
        {0.50f, 0.50f,  0.00f, 6.0f, 0.13f, 1.0f},
    }, 8, 0.66f},
};

struct Profile {
    float early_gain;
    float late_gain;
    float decay_seconds;
    float fdn_damping;
    float reflection_scale;
};

struct ViewRender {
    float cabin_mix;
    float wet_scale;
    float width;
    float gain;
    float cutoff_hz;
    float active;
};

constexpr ViewRender view_render(CabinMode mode) noexcept {
    switch (mode) {
        case CabinMode::Cockpit:   return {1.0f, 1.00f, 1.00f, 1.02f, 18000.0f, 1.0f};
        case CabinMode::Dashboard: return {1.0f, 0.84f, 0.92f, 1.04f, 18000.0f, 1.0f};
        case CabinMode::ChaseNear: return {0.0f, 0.00f, 0.58f, 1.00f, 15000.0f, 1.0f};
        case CabinMode::ChaseFar:  return {0.0f, 0.00f, 0.40f, 0.94f, 10500.0f, 1.0f};
        case CabinMode::Hood:      return {0.0f, 0.00f, 0.74f, 1.03f, 16500.0f, 1.0f};
        case CabinMode::Bumper:    return {0.0f, 0.00f, 0.62f, 1.00f, 13500.0f, 1.0f};
        default:                   return {0.0f, 0.00f, 1.00f, 1.00f, 20000.0f, 0.0f};
    }
}

constexpr std::array<Profile, static_cast<int>(CabinProfile::Count)> kProfiles{{
    {0.14f, 0.065f, 0.20f, 0.40f, 1.00f}, // Generic
    {0.17f, 0.095f, 0.30f, 0.30f, 1.10f}, // Luxury
    {0.09f, 0.038f, 0.12f, 0.54f, 0.84f}, // Race
    {0.14f, 0.070f, 0.22f, 0.38f, 1.00f}, // Saloon
    {0.12f, 0.052f, 0.17f, 0.46f, 0.92f}, // SportsCar
    {0.19f, 0.105f, 0.34f, 0.28f, 1.18f}, // Van
}};

constexpr std::array<int, CabinDSP::kNumFdnLines> kFdnLengths{{997, 1259, 1601, 1999}};
// Quantiles fitted from the non-sparse four-channel FH6 IR set. Only timing is
// borrowed: their very large low-frequency transfer gain is inappropriate for
// full-range music and is intentionally not copied into the radio EQ.
constexpr std::array<float, 6> kReflectionMs{{2.4f, 3.9f, 5.4f, 6.9f, 8.3f, 13.0f}};
constexpr std::array<float, 6> kReflectionGain{{0.30f, 0.24f, 0.19f, 0.15f, 0.11f, 0.08f}};

float softclip(float x, float ceiling) noexcept {
    if (ceiling >= 1.0f) return std::clamp(x, -1.0f, 1.0f);
    if (x > ceiling) {
        const float d = (x - ceiling) / (1.0f - ceiling);
        return ceiling + (1.0f - ceiling) * std::tanh(d);
    }
    if (x < -ceiling) {
        const float d = (-x - ceiling) / (1.0f - ceiling);
        return -ceiling - (1.0f - ceiling) * std::tanh(d);
    }
    return x;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

CabinDSP::Biquad make_biquad(int type, double f0, double db, double q) noexcept {
    constexpr double pi = 3.14159265358979323846;
    constexpr double fs = 48000.0;
    const double w0 = 2.0 * pi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double A = std::pow(10.0, db / 40.0);
    double a0 = 1.0, a1 = 0.0, a2 = 0.0, b0 = 0.0, b1 = 0.0, b2 = 0.0;
    if (type == 1) {
        const double alpha = sw / (2.0 * q);
        b0 = 1.0 + alpha * A; b1 = -2.0 * cw; b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A; a1 = -2.0 * cw; a2 = 1.0 - alpha / A;
    } else {
        const double alpha = sw / std::sqrt(2.0);
        const double sqA = std::sqrt(A);
        if (type == 0) {
            b0 = A * ((A+1) - (A-1)*cw + 2*sqA*alpha);
            b1 = 2*A * ((A-1) - (A+1)*cw);
            b2 = A * ((A+1) - (A-1)*cw - 2*sqA*alpha);
            a0 = (A+1) + (A-1)*cw + 2*sqA*alpha;
            a1 = -2*((A-1) + (A+1)*cw);
            a2 = (A+1) + (A-1)*cw - 2*sqA*alpha;
        } else {
            b0 = A * ((A+1) + (A-1)*cw + 2*sqA*alpha);
            b1 = -2*A * ((A-1) + (A+1)*cw);
            b2 = A * ((A+1) + (A-1)*cw - 2*sqA*alpha);
            a0 = (A+1) - (A-1)*cw + 2*sqA*alpha;
            a1 = 2*((A-1) - (A+1)*cw);
            a2 = (A+1) - (A-1)*cw - 2*sqA*alpha;
        }
    }
    CabinDSP::Biquad c;
    c.b0 = static_cast<float>(b0/a0); c.b1 = static_cast<float>(b1/a0);
    c.b2 = static_cast<float>(b2/a0); c.a1 = static_cast<float>(a1/a0);
    c.a2 = static_cast<float>(a2/a0);
    return c;
}

} // namespace

const char* cabin_mode_name(CabinMode mode) noexcept {
    switch (mode) {
        case CabinMode::Cockpit: return "cockpit";
        case CabinMode::Dashboard: return "dashboard";
        case CabinMode::ChaseNear: return "chase_near";
        case CabinMode::ChaseFar: return "chase_far";
        case CabinMode::Hood: return "hood";
        case CabinMode::Bumper: return "bumper";
        default: return "unknown";
    }
}

const char* cabin_profile_name(CabinProfile p) noexcept {
    switch (p) {
        case CabinProfile::Luxury: return "luxury";
        case CabinProfile::Race: return "race";
        case CabinProfile::Saloon: return "saloon";
        case CabinProfile::SportsCar: return "sportscar";
        case CabinProfile::Van: return "van";
        default: return "generic";
    }
}

const char* speaker_layout_name(SpeakerLayout layout) noexcept {
    switch (layout) {
        case SpeakerLayout::Front2: return "front2";
        case SpeakerLayout::Cabin4: return "cabin4";
        case SpeakerLayout::Surround8: return "surround8";
        default: return "premium6";
    }
}

CabinDSP::CabinDSP() {
    build_eq_banks();
    build_fdn_feedback();
}

void CabinDSP::build_eq_banks() noexcept {
    // Per-profile RBJ filter banks: low shelf, two broad modal corrections,
    // and high shelf. They intentionally describe the cabin, not a speaker EQ.
    struct E { double low, low_mid, presence, high; };
    constexpr std::array<E, static_cast<int>(CabinProfile::Count)> specs{{
        { 1.2, -0.8, -0.5, -2.0},
        { 2.0, -1.5, -1.0, -3.5},
        {-0.5,  1.0,  0.8, -1.0},
        { 1.2, -1.0, -0.8, -2.6},
        { 1.5, -0.5,  0.4, -1.8},
        { 2.5,  0.8, -1.4, -4.0},
    }};
    for (int p = 0; p < static_cast<int>(CabinProfile::Count); ++p) {
        eq_[p][0] = make_biquad(0, 85.0, specs[p].low, 0.707);
        eq_[p][1] = make_biquad(1, 240.0, specs[p].low_mid, 0.90);
        eq_[p][2] = make_biquad(1, 3000.0, specs[p].presence, 0.85);
        eq_[p][3] = make_biquad(2, 9000.0, specs[p].high, 0.707);
    }
}

void CabinDSP::build_fdn_feedback() noexcept {
    for (int p = 0; p < static_cast<int>(CabinProfile::Count); ++p) {
        for (int i = 0; i < kNumFdnLines; ++i) {
            const float delay_s = static_cast<float>(kFdnLengths[i]) / kSampleRate;
            fdn_feedback_[p][i] = std::pow(10.0f, -3.0f * delay_s / kProfiles[p].decay_seconds);
        }
    }
}

void CabinDSP::set_profile(CabinProfile p) noexcept {
    if (static_cast<int>(p) < 0 || p >= CabinProfile::Count) p = CabinProfile::Generic;
    profile_.store(p, std::memory_order_release);
}

bool CabinDSP::set_profile_name(std::string_view name) noexcept {
    for (int p = 0; p < static_cast<int>(CabinProfile::Count); ++p) {
        const auto profile = static_cast<CabinProfile>(p);
        if (iequals(name, cabin_profile_name(profile))) { set_profile(profile); return true; }
    }
    return false;
}

void CabinDSP::set_speaker_layout(SpeakerLayout layout) noexcept {
    if (static_cast<int>(layout) < 0 || static_cast<int>(layout) >= static_cast<int>(std::size(kLayouts)))
        layout = SpeakerLayout::Premium6;
    layout_.store(layout, std::memory_order_release);
}

bool CabinDSP::set_speaker_layout_name(std::string_view name) noexcept {
    constexpr std::array<SpeakerLayout, 4> all{{SpeakerLayout::Front2, SpeakerLayout::Cabin4,
                                                SpeakerLayout::Premium6, SpeakerLayout::Surround8}};
    for (auto layout : all) {
        if (iequals(name, speaker_layout_name(layout))) { set_speaker_layout(layout); return true; }
    }
    return false;
}

void CabinDSP::set_crossfeed(float v) noexcept { if (std::isfinite(v)) crossfeed_.store(std::clamp(v,0.0f,0.6f)); }
void CabinDSP::set_driver_offset(float v) noexcept { if (std::isfinite(v)) driver_offset_.store(std::clamp(v,0.0f,1.0f)); }
void CabinDSP::set_rear_fill(float v) noexcept { if (std::isfinite(v)) rear_fill_.store(std::clamp(v,0.0f,1.0f)); }
void CabinDSP::set_rear_delay_ms(float v) noexcept { if (std::isfinite(v)) rear_delay_ms_.store(std::clamp(v,0.0f,4.0f)); }
void CabinDSP::set_reflection_amount(float v) noexcept { if (std::isfinite(v)) reflection_amount_.store(std::clamp(v,0.0f,1.0f)); }
void CabinDSP::set_cabin_wet(float v) noexcept { if (std::isfinite(v)) cabin_wet_.store(std::clamp(v,0.0f,1.0f)); }
void CabinDSP::set_head_width_m(float v) noexcept { if (std::isfinite(v)) head_width_m_.store(std::clamp(v,0.12f,0.24f)); }
void CabinDSP::set_openness(float v) noexcept { if (std::isfinite(v)) openness_.store(std::clamp(v,0.0f,1.0f)); }
void CabinDSP::set_eq_enabled(bool on) noexcept { eq_enabled_.store(on); }
void CabinDSP::set_ceiling(float v) noexcept { if (std::isfinite(v)) ceiling_.store(std::clamp(v,0.5f,1.0f)); }

void CabinDSP::reset() noexcept {
    for (auto& ch : source_delay_) std::fill(std::begin(ch), std::end(ch), 0.0f);
    for (auto& s : hrtf_lp_) { s[0] = 0.0f; s[1] = 0.0f; }
    for (auto& line : fdn_) std::fill(std::begin(line), std::end(line), 0.0f);
    std::fill(std::begin(fdn_pos_), std::end(fdn_pos_), 0);
    std::fill(std::begin(fdn_lp_), std::end(fdn_lp_), 0.0f);
    for (auto& stage : bq_z1_) { stage[0] = 0.0f; stage[1] = 0.0f; }
    for (auto& stage : bq_z2_) { stage[0] = 0.0f; stage[1] = 0.0f; }
    source_pos_ = 0;
    openness_state_ = openness_.load(std::memory_order_acquire);
    const auto view = view_render(mode_.load(std::memory_order_acquire));
    mode_mix_state_ = view.active;
    view_gain_state_ = view.gain;
    view_width_state_ = view.width;
    view_cutoff_state_ = view.cutoff_hz;
    exterior_lp_[0] = exterior_lp_[1] = 0.0f;
}

float CabinDSP::source_read(int channel, float delay) const noexcept {
    delay = std::clamp(delay, 1.0f, static_cast<float>(kSourceDelayLen - 2));
    float index = static_cast<float>(source_pos_) - delay;
    int i0 = static_cast<int>(std::floor(index));
    const float frac = index - static_cast<float>(i0);
    const float a = source_delay_[channel][i0 & kSourceDelayMask];
    const float b = source_delay_[channel][(i0 + 1) & kSourceDelayMask];
    return a + frac * (b - a);
}

float CabinDSP::run_eq(int profile_index, int channel, float x) noexcept {
    for (int s = 0; s < kNumEqStages; ++s) {
        const Biquad& c = eq_[profile_index][s];
        const float y = c.b0*x + bq_z1_[s][channel];
        bq_z1_[s][channel] = c.b1*x - c.a1*y + bq_z2_[s][channel];
        bq_z2_[s][channel] = c.b2*x - c.a2*y;
        x = y;
    }
    return x;
}

float CabinDSP::run_head_shadow(int speaker, int ear, float x, float alpha) noexcept {
    float& z = hrtf_lp_[speaker][ear];
    z += alpha * (x - z);
    return z;
}

void CabinDSP::process(float& left, float& right) noexcept {
    left = std::clamp(left, -4.0f, 4.0f);
    right = std::clamp(right, -4.0f, 4.0f);

    const float dry_l = left;
    const float dry_r = right;

    const CabinMode mode = mode_.load(std::memory_order_relaxed);
    const ViewRender view = view_render(mode);
    const int profile_index = std::clamp(static_cast<int>(profile_.load(std::memory_order_relaxed)),
                                         0, static_cast<int>(CabinProfile::Count)-1);
    const int layout_index = std::clamp(static_cast<int>(layout_.load(std::memory_order_relaxed)), 0, 3);
    const Layout& layout = kLayouts[layout_index];
    const Profile& profile = kProfiles[profile_index];
    const bool binaural = binaural_.load(std::memory_order_relaxed);
    const float driver = driver_offset_.load(std::memory_order_relaxed);
    const float rear_fill = rear_fill_.load(std::memory_order_relaxed);
    const float rear_delay = rear_delay_ms_.load(std::memory_order_relaxed);
    const float crossfeed = crossfeed_.load(std::memory_order_relaxed);
    const float head_width = head_width_m_.load(std::memory_order_relaxed);
    const bool use_eq = eq_enabled_.load(std::memory_order_relaxed);
    const float ceiling = ceiling_.load(std::memory_order_relaxed);

    const float open_target = openness_.load(std::memory_order_relaxed);
    openness_state_ += 0.00042f * (open_target - openness_state_); // ~50 ms
    const float openness = std::clamp(openness_state_, 0.0f, 1.0f);

    const float input_l = left + crossfeed * right;
    const float input_r = right + crossfeed * left;
    source_delay_[0][source_pos_] = input_l;
    source_delay_[1][source_pos_] = input_r;

    float direct_l = 0.0f, direct_r = 0.0f;
    const float max_itd = (head_width / kSpeedOfSound) * kSampleRate;
    for (int i = 0; i < layout.count; ++i) {
        const Speaker& s = layout.speakers[i];
        const float side = s.sin_azimuth;
        const float driver_delay = side * driver * 0.55f; // ms, left side arrives sooner
        const float delay_ms = s.delay_ms + driver_delay + s.rear * rear_delay;
        const float base_delay = std::max(1.0f, delay_ms * 48.0f);
        const float rear_gain = 1.0f - s.rear * (1.0f - rear_fill);
        const float driver_gain = std::clamp(1.0f - side * driver * 0.10f, 0.80f, 1.20f);
        const float gain = s.gain * rear_gain * driver_gain;

        if (!binaural) {
            const float src = s.mix_l * source_read(0, base_delay) + s.mix_r * source_read(1, base_delay);
            const float pan = std::clamp(0.5f * (side + 1.0f), 0.0f, 1.0f);
            direct_l += src * gain * std::sqrt(1.0f - pan);
            direct_r += src * gain * std::sqrt(pan);
        } else {
            const float itd = std::abs(side) * max_itd;
            const float delay_l = base_delay + (side > 0.0f ? itd : 0.0f);
            const float delay_r = base_delay + (side < 0.0f ? itd : 0.0f);
            float src_l = s.mix_l * source_read(0, delay_l) + s.mix_r * source_read(1, delay_l);
            float src_r = s.mix_l * source_read(0, delay_r) + s.mix_r * source_read(1, delay_r);
            const float lateral = std::abs(side);
            const float far_cut = 7200.0f - 5200.0f * lateral;
            const float near_alpha = 1.0f - std::exp(-2.0f*kPi*12000.0f/kSampleRate);
            const float far_alpha = 1.0f - std::exp(-2.0f*kPi*far_cut/kSampleRate);
            const bool left_far = side > 0.0f;
            src_l = run_head_shadow(i, 0, src_l, left_far ? far_alpha : near_alpha);
            src_r = run_head_shadow(i, 1, src_r, left_far ? near_alpha : far_alpha);
            const float gain_l = 0.78f - 0.22f*side;
            const float gain_r = 0.78f + 0.22f*side;
            direct_l += src_l * gain * gain_l;
            direct_r += src_r * gain * gain_r;
        }
    }
    direct_l *= layout.normalization;
    direct_r *= layout.normalization;

    // Keep the real stereo source as a stable perceptual anchor. A fully wet
    // sum of several 2-6 ms virtual-speaker copies creates obvious combing on
    // music and was the main reason the prototype sounded like a crude filter.
    const float spatial_mix = binaural ? 0.88f : 0.32f;
    direct_l = dry_l * (1.0f - spatial_mix) + direct_l * spatial_mix;
    direct_r = dry_r * (1.0f - spatial_mix) + direct_r * spatial_mix;

    // Sparse, asymmetric early reflection field. Timing is scaled by profile;
    // opposite signs on alternating cross terms preserve spaciousness without
    // collapsing every reflection to correlated stereo.
    float early_l = 0.0f, early_r = 0.0f;
    for (int i = 0; i < static_cast<int>(kReflectionMs.size()); ++i) {
        const float d = kReflectionMs[i] * profile.reflection_scale * 48.0f;
        const float a = source_read(0, d);
        const float b = source_read(1, d + (i & 1 ? 7.0f : 13.0f));
        const float g = kReflectionGain[i];
        if (i & 1) {
            early_l += g * (0.70f*a - 0.30f*b);
            early_r += g * (0.70f*b + 0.30f*a);
        } else {
            early_l += g * (0.82f*a + 0.18f*b);
            early_r += g * (0.82f*b - 0.18f*a);
        }
    }
    source_pos_ = (source_pos_ + 1) & kSourceDelayMask;

    // Four mutually coupled delay lines (Householder feedback matrix). The
    // low-pass inside each loop yields frequency-dependent decay and a dense,
    // decorrelated tail at a fraction of long FIR convolution cost.
    float y[kNumFdnLines]{};
    float sum = 0.0f;
    for (int i = 0; i < kNumFdnLines; ++i) { y[i] = fdn_[i][fdn_pos_[i]]; sum += y[i]; }
    const float mono_in = 0.5f * (input_l + input_r);
    const float feedback_open = 1.0f - 0.46f * openness;
    for (int i = 0; i < kNumFdnLines; ++i) {
        const float mixed = y[i] - 0.5f * sum;
        fdn_lp_[i] += profile.fdn_damping * (mixed - fdn_lp_[i]);
        const float injection = mono_in * (i & 1 ? -0.31f : 0.31f);
        fdn_[i][fdn_pos_[i]] = injection + fdn_lp_[i] * fdn_feedback_[profile_index][i] * feedback_open;
        if (++fdn_pos_[i] >= kFdnLengths[i]) fdn_pos_[i] = 0;
    }
    const float late_l = 0.25f * (y[0] + y[1] - y[2] - y[3]);
    const float late_r = 0.25f * (y[0] - y[1] + y[2] - y[3]);

    float wet = cabin_wet_.load(std::memory_order_relaxed);
    float refl = reflection_amount_.load(std::memory_order_relaxed);
    wet *= (1.0f - 0.68f * openness) * view.wet_scale;

    float out_l = direct_l + wet * (refl * profile.early_gain * early_l + profile.late_gain * late_l);
    float out_r = direct_r + wet * (refl * profile.early_gain * early_r + profile.late_gain * late_r);

    if (use_eq) {
        const float eq_l = run_eq(profile_index, 0, out_l);
        const float eq_r = run_eq(profile_index, 1, out_r);
        // An open roof weakens the closed-cavity coloration. The slow openness
        // smoother also makes this coefficient transition click-free.
        const float eq_mix = 0.68f * (1.0f - 0.72f * openness);
        out_l += eq_mix * (eq_l - out_l);
        out_r += eq_mix * (eq_r - out_r);
    }

    // The four exterior cameras hear a distance-dependent, progressively
    // narrower car source instead of sharing one unity bypass.  This is kept
    // deliberately mild because FH6 already spatializes the vehicle itself.
    view_gain_state_ += 0.00052f * (view.gain - view_gain_state_);
    view_width_state_ += 0.00052f * (view.width - view_width_state_);
    view_cutoff_state_ += 0.00052f * (view.cutoff_hz - view_cutoff_state_);
    const float mid = 0.5f * (dry_l + dry_r);
    const float side = 0.5f * (dry_l - dry_r) * view_width_state_;
    const float ext_in_l = mid + side;
    const float ext_in_r = mid - side;
    const float lp_alpha = 1.0f - std::exp(-2.0f * kPi * view_cutoff_state_ / kSampleRate);
    exterior_lp_[0] += lp_alpha * (ext_in_l - exterior_lp_[0]);
    exterior_lp_[1] += lp_alpha * (ext_in_r - exterior_lp_[1]);
    const float exterior_l = exterior_lp_[0] * view_gain_state_;
    const float exterior_r = exterior_lp_[1] * view_gain_state_;

    const float cabin_mid = 0.5f * (out_l + out_r);
    const float cabin_side = 0.5f * (out_l - out_r) * view_width_state_;
    const float cabin_l = (cabin_mid + cabin_side) * view_gain_state_;
    const float cabin_r = (cabin_mid - cabin_side) * view_gain_state_;
    const float target_l = view.cabin_mix * cabin_l + (1.0f - view.cabin_mix) * exterior_l;
    const float target_r = view.cabin_mix * cabin_r + (1.0f - view.cabin_mix) * exterior_r;

    // View changes crossfade over about 40 ms. Unknown is the only true bypass.
    mode_mix_state_ += 0.00052f * (view.active - mode_mix_state_);
    const float mix = std::clamp(mode_mix_state_, 0.0f, 1.0f);
    const float processed_l = softclip(target_l, ceiling);
    const float processed_r = softclip(target_r, ceiling);
    left = dry_l + mix * (processed_l - dry_l);
    right = dry_r + mix * (processed_r - dry_r);
}

} // namespace fh6r
