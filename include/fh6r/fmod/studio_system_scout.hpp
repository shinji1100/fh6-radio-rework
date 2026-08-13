#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <cstddef>

namespace fh6r::fmod {

// Trace the System::create call site's RCX source (the Studio::System** out-slot
// address), read the base value, apply the accumulated displacement, and read
// the Studio::System* handle out of that slot.
//
// Read-only with respect to game code/data: it resolves addresses, reads .text,
// and reads the handle slot — never calls the Studio API and never writes.
// Returns the Studio System pointer, or nullptr if it cannot be resolved or the
// slot is not yet populated (i.e. System::create has not run yet at the time of
// the call). Safe to call repeatedly, so callers can retry at a later moment.
std::byte* locate_studio_system_handle(const PEImage& img) noexcept;

// Step 2 (continued): locate the Studio System handle and verify its identity.
// Resolves System::create / getCoreSystem / getBankCount / getBankList, scans
// .text for the create() xref, traces RCX -> handle slot, reads the handle, and
// reports getCoreSystem (Core identity) + getBankCount (FMOD_OK + loaded-bank
// count). Read-only; retries briefly because System::create may run a moment
// after DLL load. Only FMOD read queries are issued.
void scout_studio_system(const PEImage& img) noexcept;

} // namespace fh6r::fmod
