#pragma once
#include "fh6r/fmod/pe_image.hpp"

namespace fh6r::fmod {

// Phase 1 (interior/exterior state discovery) — observation hook.
//
// The FMOD Studio parameter getters (getParameterByID / getParameterByName)
// have been dead-code-eliminated from this build, but the setters are still
// called by the game. From the on-disk MasterBank.strings.bank we already know
// the interior/exterior switch is a global parameter named "Cockpit".
//
// This patches the Studio::System vtable entry for setParameterByID (DATA, not
// CODE — no prologue relocation) so every call is observed, then the hook
// delegates straight back to the real function. It logs each parameter
// id/value change so we can confirm "Cockpit" flips on cockpit<->chase.
//
// NOTE: a previous revision relocated the setter prologue into a trampoline
// that was entered via CALL; that broke "mov [rsp+disp], reg" prologues (they
// assumed the original entry RSP) and crashed the game. Vtable patching avoids
// relocation entirely: the hook is entered by a normal virtual call and calls
// the original function directly.
//
// Fail-safe: the setter is only hooked when it resolves to a unique function
// whose vtable slot is unambiguous and plausibly a vtable entry; otherwise the
// candidates/slots are logged and nothing is patched.
void install_studio_param_hooks(const PEImage& img) noexcept;

} // namespace fh6r::fmod
