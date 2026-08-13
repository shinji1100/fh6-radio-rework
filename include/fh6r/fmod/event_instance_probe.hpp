#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <cstddef>

namespace fh6r::fmod {

// Observation-only: dump the vtable of `radio_stream` (the object embedded at
// RadioStreamFmod + 0x10, whose first 8 bytes look like a vtable) and annotate
// which entries equal the anchor-resolved EventInstance API addresses.
//
// This does NOT assert the object's identity. FMOD Studio API types are packed
// handles, not vtable-polymorphic C++ objects, so a vtable here is more
// consistent with an FH6 wrapper than with an EventInstance. The dump is only a
// data point for later locating the wrapper field that holds the real
// EventInstance handle. Read-only; no object method is invoked.
bool probe_event_instance(const PEImage& img, std::byte* radio_stream) noexcept;

} // namespace fh6r::fmod
