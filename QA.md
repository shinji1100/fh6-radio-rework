# QA status for v0.1.0

Validated in the current build environment:

- Portable CMake configuration succeeds with `FH6R_PORTABLE_TESTS_ONLY=ON`.
- SPSC ring tests pass, including concurrent producer/consumer ordering and consumer-owned reset semantics.
- DSP channel-contract tests pass: output channels are never upgraded beyond FMOD's supplied layout.
- Drift-controller tests pass for nominal, high-fill, low-fill and bounded-correction cases.
- Static audit passes: no LAN-wide bind, wildcard CORS, polling `Sleep(5)`, D3D12 hook, Kiero, MinHook, or legacy `force_stereo_audio` path in `src/` or `include/`.
- `src/config.cpp` passes GCC C++20 syntax/warning compilation with `-Wall -Wextra -Wpedantic`.

Not yet validated in this environment:

- Windows x64 compilation of `version.dll` (the current container has no Windows SDK/MinGW headers).
- Runtime injection into a live FH6 process.
- Long-duration QQ Music + VB-CABLE playback in FH6.

The included GitHub Actions `windows-x64` job is intended to perform the real Windows build once the repository is pushed.
