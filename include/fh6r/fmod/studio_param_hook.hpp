#pragma once
#include "fh6r/fmod/pe_image.hpp"

namespace fh6r::fmod {

// Phase 1 (interior/exterior state discovery) — READ-ONLY scout.
//
// Two hooking attempts (prologue-relocation detour, then vtable patch) both
// crashed the game and have been REMOVED:
//   1. prologue-relocation trampoline entered via CALL broke "mov [rsp+disp],reg"
//      prologues (they assume the original entry RSP);
//   2. vtable patch scanned .text for an 8-byte word equal to the setter address
//      and matched code bytes inside the decrypted FMOD image, corrupting code.
//
// This module now only logs the resolved setter candidates (addresses +
// prologues) for reference. No code or data is modified. The interior/exterior
// state ("Cockpit" global parameter) will instead be obtained by a read-only
// approach (memory diff of the camera state, or reading the Studio::System
// parameter table) which cannot crash the game.
void install_studio_param_hooks(const PEImage& img) noexcept;

} // namespace fh6r::fmod
