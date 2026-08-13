#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <cstddef>

namespace fh6r::fmod {

// Verify whether `radio_stream` (the polymorphic object embedded at
// RadioStreamFmod + 0x10, whose first 8 bytes are a vtable) is an FMOD Studio
// EventInstance.
//
// It resolves a handful of EventInstance methods by anchor+LEA (dispatch
// filtered), reads the object's vtable pointer, dumps the first 64 entries and
// flags every entry whose address equals a resolved EventInstance method.
// Read-only: it never calls into the object, so a wrong guess cannot crash.
//
// Returns true when >=2 known EventInstance methods appear in the vtable
// (i.e. radio_stream is very likely an EventInstance). The per-entry log also
// records the vtable offset of getChannelGroup, which is what the next step
// uses to call EventInstance::getChannelGroup() by stable vtable index.
bool probe_event_instance(const PEImage& img, std::byte* radio_stream) noexcept;

} // namespace fh6r::fmod
