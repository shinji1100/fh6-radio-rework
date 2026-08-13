#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <cstddef>

namespace fh6r::fmod {

struct StudioSystemSnapshot {
    std::byte* handle = nullptr;
    void* core_system = nullptr;
    bool valid = false;
};

// Trace the System::create call site's RCX source (the Studio::System** out-slot
// address), read the base value, apply the accumulated displacement, and read
// the opaque Studio System handle value out of that slot. The C API spells the
// value FMOD_STUDIO_SYSTEM*, but it is an opaque handle and must not be
// dereferenced by the scout.
//
// Read-only with respect to game code/data: it resolves addresses, reads .text,
// and reads the handle slot — never calls the Studio API and never writes.
// Returns the Studio System pointer, or nullptr if it cannot be resolved or the
// slot is not yet populated (i.e. System::create has not run yet at the time of
// the call). Safe to call repeatedly, so callers can retry at a later moment.
std::byte* locate_studio_system_handle(const PEImage& img) noexcept;

// Latest verified identity captured by scout_studio_system(). `valid` means
// getCoreSystem(handle) returned FMOD_OK and a non-null Core System. The
// controller compares that Core pointer with resolve_fmod_system() after a
// RadioStreamFmod attach to close the identity loop without another API call.
StudioSystemSnapshot studio_system_snapshot() noexcept;

// Step 2 (continued): locate the Studio System handle and verify its identity.
// Resolves System::create / getCoreSystem / getBankCount / getBankList, scans
// .text for the create() xref, traces RCX -> handle slot, reads the handle, and
// reports getCoreSystem (Core identity). Name-anchor ownership alone is not
// enough to establish the ABI of getBankCount/getBankList in this statically
// linked build, so those candidates are reported but never called. Read-only;
// retries briefly because System::create may run a moment after DLL load.
void scout_studio_system(const PEImage& img) noexcept;

} // namespace fh6r::fmod
