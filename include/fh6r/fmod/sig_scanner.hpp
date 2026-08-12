#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <string_view>
namespace fh6r::fmod {
std::byte* find_by_anchor(const PEImage& img, std::string_view anchor, std::string_view pattern) noexcept;
std::byte* find_by_pattern(const PEImage& img, std::string_view pattern) noexcept;
} // namespace fh6r::fmod
