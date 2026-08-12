# Changelog

## 0.1.0

- Rebuilt External Audio around event-driven WASAPI loopback.
- Added dedicated MMCSS capture thread.
- Replaced mutex/vector staging with a frame-oriented SPSC ring.
- Added bounded clock-drift compensation for long-running capture.
- Fixed FMOD output-buffer handling so the callback never increases the allocated channel count.
- Added consumer-owned ring reset and prebuffer/rebuffer recovery.
- Removed arbitrary radio-instance fallback.
- Replaced the hard-coded Peter Broderick carrier with automatic active `RadioStreamFmod` selection.
- Added a `RadioState` Streamer Mode gate so normal FH6 stations are never targeted intentionally.
- Added localhost-only native Chinese dashboard and diagnostics.
- Removed D3D12 artwork hooks and non-core streaming integrations from the initial release.
- Added portable tests for ring concurrency, DSP channel contract and drift controller.
