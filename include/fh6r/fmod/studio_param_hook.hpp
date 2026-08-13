#pragma once
#include "fh6r/fmod/pe_image.hpp"

namespace fh6r::fmod {

// Phase 1 (interior/exterior state discovery) — read-only observation hook.
//
// The FMOD Studio parameter getters (getParameterByID / getParameterByName)
// have been dead-code-eliminated from this build (their debug name strings are
// absent), but the setters (setParameterByName / setParameterByID) are still
// called by the game. We already know from the on-disk MasterBank.strings.bank
// that the interior/exterior switch is a global parameter named "Cockpit".
//
// This installs a minimal in-process detour on the two setters and logs every
// parameter NAME + VALUE transition (and sampled ID + VALUE for the by-ID setter)
// so we can confirm "Cockpit" flips 0<->1 on cockpit<->chase, and observe
// CockpitGainReduction / CockpitFiltering etc.
//
// Fail-safe by design: the detour relocates only position-independent prologue
// bytes (a conservative x86-64 length decoder bails on any RIP-relative /
// branch / unknown opcode). If a function's prologue cannot be relocated, that
// hook is skipped and the prologue is logged for the next iteration — the game
// keeps running untouched. Read-only with respect to game state (nothing is
// modified; the original function is always invoked).
void install_studio_param_hooks(const PEImage& img) noexcept;

} // namespace fh6r::fmod
