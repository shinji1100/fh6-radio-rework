# Upstream notice

FH6 Radio Rework v0.1 is a GPL-3.0 derivative of portions of:

- Project: `g0ldyy/fh6-universal-radio`
- Upstream snapshot referenced during the rewrite: `bfaf3df3eb80e1885f849f039faaa09051d919b8`
- License: GNU General Public License v3.0

Derived/reworked low-level areas include:

- `version.dll` export proxy pattern
- PE image parsing
- FMOD signature/anchor discovery
- `RadioStreamFmod` RTTI and heap discovery
- FMOD DSP attachment strategy and known Streamer Mode carrier identifier

Substantially rewritten areas in this branch include:

- event-driven WASAPI capture
- allocation-free adaptive resampling hot path
- clock-drift compensation
- frame-oriented SPSC ring
- FMOD output-buffer contract handling and rebuffer logic
- localhost-only Chinese dashboard and diagnostics
- simplified controller architecture

See `LICENSE` for GPL-3.0 terms.
