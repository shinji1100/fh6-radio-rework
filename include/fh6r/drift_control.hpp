#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace fh6r::drift {
inline double resample_step(std::uint32_t input_rate, std::uint32_t output_rate,
                            std::size_t readable_frames, std::size_t capacity_frames) noexcept {
    if (!input_rate || !output_rate) return 1.0;
    constexpr double max_correction = 0.005;
    const double cap = static_cast<double>(capacity_frames);
    const double fill = static_cast<double>(readable_frames);
    const double target = cap * 0.25;
    const double error = cap > 0.0 ? (fill - target) / cap : 0.0;
    const double correction = std::clamp(error * 0.04, -max_correction, max_correction);
    return (static_cast<double>(input_rate) / static_cast<double>(output_rate)) * (1.0 + correction);
}
}
