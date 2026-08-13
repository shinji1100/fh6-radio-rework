#pragma once
#include "fh6r/fmod/pe_image.hpp"

namespace fh6r::fmod {

// Step 1 of the Studio enumeration route: resolve the Studio API surface and
// report, per anchor, whether it is MISSING (0 candidates), RESOLVED (1) or
// AMBIGUOUS (>1) after the shared dispatch/logging function is filtered out.
//
// This does NOT call any of the APIs and does NOT inspect object memory /
// vtables. The only thing validated here is that each function address resolves
// reliably (FMOD Studio objects are packed handles, so object identity is not
// part of this step). Read-only, runs once at startup.
void scout_studio_api(const PEImage& img) noexcept;

} // namespace fh6r::fmod
