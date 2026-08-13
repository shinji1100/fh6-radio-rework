#pragma once
#include <string>

namespace fh6r::fmod {
struct FMODFns;
}

namespace fh6r::fmod {

// Read-only scout: walk the FMOD channel-group tree from the master group,
// find convolution-reverb DSPs (type 32), and dump their IR parameter data.
//
// For each convolution DSP found it logs the wet/dry/linked levels, reads the
// IR via getParameterData(0), writes the raw IR bytes to out_dir as
// "cockpit_ir_<n>.raw", and prints an energy profile so the offline acoustic
// analysis can answer: mono vs stereo, length, direct/onset location, and how
// energy is distributed across 0-5 / 5-20 / 20-50 / 50-100 ms.
//
// Every FMOD call is wrapped in seh_call and the walk is depth/breadth limited,
// so a bad ABI guess cannot take the game down. Returns true iff >= 1
// convolution DSP was found and dumped.
bool scout_cockpit_reverb(const FMODFns& fns, void* system,
                          const std::string& out_dir) noexcept;

} // namespace fh6r::fmod
