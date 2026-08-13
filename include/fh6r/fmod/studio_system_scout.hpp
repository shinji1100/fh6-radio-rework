#pragma once
#include "fh6r/fmod/pe_image.hpp"

namespace fh6r::fmod {

// Step 2: locate the Studio::System handle storage by resolving the Studio
// factory / lifecycle methods (System::create, System::getCoreSystem,
// System::initialize, System::update) and scanning .text for direct call xrefs
// from FH6 code. For each xref it dumps the preceding bytes so the RCX source
// (the Studio System out-parameter / handle) can be traced.
//
// Read-only: it resolves addresses and reads .text, never calls the APIs and
// never writes. Runs once at startup.
void scout_studio_system(const PEImage& img) noexcept;

} // namespace fh6r::fmod
