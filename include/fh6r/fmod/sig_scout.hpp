#pragma once
#include "fh6r/fmod/pe_image.hpp"

namespace fh6r::fmod {

// Phase 1 audio-state probe foundation.
//
// The core path resolves only the handful of FMOD APIs it needs (createDSP /
// release / addDSP / removeDSP / handle-resolver). The cabin-acoustics target
// needs a much wider FMOD surface: listener 3D attributes, the active radio
// channel's 3D level / mix matrix, the channel-group hierarchy and the DSP
// chain hanging off every level. Each of those is a separate FMOD function
// whose exact byte prologue is build-specific and must not be guessed.
//
// scout_apis() resolves the *candidates* for every desired API by anchor
// (the FMOD debug name string in .rdata, e.g. "System::get3DListenerAttributes"),
// then logs each candidate's address plus its first 32 prologue bytes. One run
// on the target machine yields the empirical byte patterns that the real
// state enumerator (next iteration) will plug into find_by_anchor.
//
// Safe to call at startup: read-only, VirtualQuery-guarded, mutates nothing.
void scout_apis(const PEImage& img) noexcept;

} // namespace fh6r::fmod
