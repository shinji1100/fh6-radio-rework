#pragma once
#include "fh6r/fmod/pe_image.hpp"

namespace fh6r::fmod {

// Phase 1 (Studio global parameter path), step 1.
//
// The low-level radio channel carried no interior/exterior state (confirmed by
// the audio-state probe), so the signal is expected to live in FMOD Studio's
// global parameter space. That requires the FMOD Studio System object, which
// the game holds somewhere private. We locate it indirectly: the Studio System
// vtable is a static table in .rdata whose entries point at the Studio methods
// (getParameterDescriptionList, setListenerAttributes, ...), so we find the
// vtable entry for one resolved method, then scan the heap for an object whose
// first 8 bytes point at that vtable. Same technique the radio discovery uses
// for RadioStreamFmod instances.

// Given the runtime address of one resolved Studio::System method (a vtable
// entry), find and return the Studio::System object pointer (or nullptr).
void* locate_studio_system(const PEImage& img, std::byte* method_fn) noexcept;

// Resolve the Studio getters needed for parameter enumeration and log the
// Studio::System object + vtable dump. Runs once at startup, read-only.
void studio_scout(const PEImage& img) noexcept;

} // namespace fh6r::fmod
