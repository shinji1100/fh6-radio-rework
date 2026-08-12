#pragma once
#include <algorithm>
#include <cstdint>

namespace fh6r::dsp_contract {
inline std::int32_t safe_channel_count(std::int32_t input_channels,
                                       const std::int32_t* fmod_out_channels) noexcept {
    const std::int32_t channels = (fmod_out_channels && *fmod_out_channels > 0)
        ? *fmod_out_channels : input_channels;
    return (channels > 0 && channels <= 32) ? channels : 0;
}

inline void write_frame(float* dst, std::int32_t channels, float left, float right,
                        bool native_stereo) noexcept {
    if (!dst || channels <= 0) return;
    left = std::clamp(left, -1.0f, 1.0f);
    right = std::clamp(right, -1.0f, 1.0f);
    const float mono = (left + right) * 0.5f;
    if (native_stereo && channels >= 2) {
        dst[0] = left;
        dst[1] = right;
        for (std::int32_t c = 2; c < channels; ++c) dst[c] = mono;
    } else {
        for (std::int32_t c = 0; c < channels; ++c) dst[c] = mono;
    }
}
} // namespace fh6r::dsp_contract
