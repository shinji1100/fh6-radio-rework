#pragma once

#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include "fh6r/fmod/radio_discovery.hpp"

namespace fh6r::fmod {

// Read-only identity and dimensionality probe for the active Streamer Mode
// stream. It cross-checks the wrapper's channel/sound handles through FMOD Core
// and, when a direct EventInstance handle is present, asks Studio for playback,
// is3D and isStream. Every call is SEH-guarded and logged with FMOD_RESULT.
void probe_radio_acoustics(const PEImage& img, const RadioInstance& radio,
                           const FMODFns& fns) noexcept;

} // namespace fh6r::fmod
