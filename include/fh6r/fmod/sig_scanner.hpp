#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <string_view>
namespace fh6r::fmod {
std::byte* find_by_anchor(const PEImage& img, std::string_view anchor, std::string_view pattern) noexcept;
std::byte* find_by_pattern(const PEImage& img, std::string_view pattern) noexcept;

// Signature scout: like find_by_anchor but WITHOUT a confirming byte pattern.
// Returns every candidate function that references `anchor` via `lea reg,[rip+disp]`,
// so the caller can log their addresses and prologue bytes and derive real
// patterns empirically. Used by the Phase 1 audio-state probe to resolve the
// extra FMOD APIs (listener attrs, channel 3D, DSP chain walk, ...) that the
// core path does not need.
std::vector<std::byte*> scout_anchor(const PEImage& img, std::string_view anchor) noexcept;

// Resolve a function by anchor + LEA, requiring EXACTLY one candidate.
// Returns the unique function pointer, or nullptr (and logs a warning) when the
// anchor is absent or ambiguous. This is the preferred resolver for the extra
// FMOD APIs resolved by the Phase 1 scout: in this build each of them has a
// single LEA-referencing function, so no byte pattern (which would be
// non-unique across FMOD's shared prologues) is needed.
std::byte* resolve_by_anchor_unique(const PEImage& img, std::string_view anchor) noexcept;
} // namespace fh6r::fmod
