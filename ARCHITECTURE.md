# Architecture

## Real-time path

```text
Windows playback endpoint
  -> WASAPI shared-mode loopback (event driven, MMCSS Pro Audio)
  -> format decode + adaptive linear resampler
  -> SPSC StereoFrame ring
  -> FMOD DSP mixer callback
  -> FH6 radio bus
```

The capture thread is the sole producer. The FMOD mixer callback is the sole consumer.
Control threads never move the ring read index; they only request a reset, which the
consumer applies at the next callback boundary.

## Clock drift

Nominally identical 48 kHz clocks are not physically identical. The resampler therefore
applies a bounded ±0.5% ratio correction based on ring fill. The goal is not time-stretch
quality; it is to prevent slow buffer drift while remaining inaudible for a game-radio path.

## FMOD contract

The DSP callback treats FMOD's advertised output-channel count as an upper bound and never
increases it. Default output is mono content duplicated into the already allocated channel
layout. Experimental native stereo only uses left/right when FMOD already supplied at least
two channels.

On attach or underrun the callback prebuffers 2048 frames before resuming. A full producer
ring requests a consumer-side reset, so long pauses do not resume with stale audio.

## Runtime discovery

The low-level discovery layer is derived from `g0ldyy/fh6-universal-radio` and resolves the
known Streamer Mode carrier at runtime. The controller only accepts the exact carrier
`HZ6_R9_PeterBroderick_EyesClosedandTraveling`; there is deliberately no fallback to an
arbitrary native radio instance.

## Dashboard

The dashboard is a small embedded HTTP server bound only to `127.0.0.1`. It exposes device
selection, capture start/stop, gain, the experimental stereo toggle, and diagnostics. It has
request-size limits, socket timeouts, Host validation and same-origin checks for POSTs.
