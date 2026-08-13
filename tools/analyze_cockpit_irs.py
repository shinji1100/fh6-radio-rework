#!/usr/bin/env python3
"""Reproducible, dependency-light profiling for FH6 multichannel IR WAVs.

The classifier is deliberately descriptive. It does not claim that a file is
music, engine, impact, or chassis audio without a runtime mapping. Its purpose
is to identify which responses have cabin-like early/late energy and to derive
bounded parameters for the real-time CabinDSP model.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import wave
from pathlib import Path

import numpy as np


BANDS_HZ = ((20, 80), (80, 250), (250, 1000), (1000, 4000), (4000, 12000), (12000, 24000))


def db10(value: float, floor: float = -120.0) -> float:
    return max(floor, 10.0 * math.log10(max(value, 1e-20)))


def read_pcm16(path: Path) -> tuple[int, np.ndarray]:
    with wave.open(str(path), "rb") as wav:
        if wav.getsampwidth() != 2 or wav.getcomptype() != "NONE":
            raise ValueError(f"{path.name}: expected uncompressed PCM16")
        sample_rate = wav.getframerate()
        channels = wav.getnchannels()
        raw = wav.readframes(wav.getnframes())
    pcm = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    return sample_rate, (pcm.reshape(-1, channels) / 32768.0)


def decay_time_ms(frame_energy: np.ndarray, sample_rate: int, lo_db: float, hi_db: float, scale: float) -> float | None:
    schroeder = np.cumsum(frame_energy[::-1])[::-1]
    if schroeder.size < 8 or schroeder[0] <= 0:
        return None
    decay = 10.0 * np.log10(np.maximum(schroeder / schroeder[0], 1e-14))
    mask = (decay <= lo_db) & (decay >= hi_db)
    idx = np.flatnonzero(mask)
    if idx.size < 16:
        return None
    x = idx / float(sample_rate)
    slope, _ = np.polyfit(x, decay[idx], 1)
    if slope >= -1e-6:
        return None
    return float((-60.0 / slope) * scale * 1000.0)


def analyze(path: Path) -> dict[str, object]:
    sample_rate, samples = read_pcm16(path)
    frames, channels = samples.shape
    abs_samples = np.abs(samples)
    channel_peaks = abs_samples.max(axis=0)
    peak = float(channel_peaks.max(initial=0.0))
    if peak <= 0.0:
        raise ValueError(f"{path.name}: silent IR")

    onset_threshold = max(peak * 0.01, 1.0 / 32768.0)
    channel_onsets = []
    for channel in range(channels):
        hits = np.flatnonzero(abs_samples[:, channel] >= onset_threshold)
        channel_onsets.append(int(hits[0]) if hits.size else frames)
    onset = min(channel_onsets)
    response = samples[onset:]
    frame_energy = np.sum(response * response, axis=1)
    total_energy = float(frame_energy.sum())

    def energy_between(start_ms: float, end_ms: float | None) -> float:
        start = min(len(frame_energy), int(round(start_ms * sample_rate / 1000.0)))
        end = len(frame_energy) if end_ms is None else min(len(frame_energy), int(round(end_ms * sample_rate / 1000.0)))
        return float(frame_energy[start:end].sum()) / max(total_energy, 1e-20)

    e0_5 = energy_between(0.0, 5.0)
    e5_20 = energy_between(5.0, 20.0)
    e20_50 = energy_between(20.0, 50.0)
    e50_100 = energy_between(50.0, 100.0)
    e100_plus = energy_between(100.0, None)
    early = e0_5 + e5_20
    late = e20_50 + e50_100 + e100_plus

    nfft = 1 << max(12, int(math.ceil(math.log2(max(2, len(response))))))
    spectrum = np.fft.rfft(response, n=nfft, axis=0)
    power = np.sum(np.abs(spectrum) ** 2, axis=1)
    frequencies = np.fft.rfftfreq(nfft, 1.0 / sample_rate)
    spectral_total = float(power[(frequencies >= 20.0) & (frequencies <= min(24000.0, sample_rate / 2.0))].sum())
    band_ratios = {}
    for low, high in BANDS_HZ:
        high = min(high, sample_rate / 2.0)
        mask = (frequencies >= low) & (frequencies < high)
        band_ratios[f"band_{low}_{int(high)}_pct"] = 100.0 * float(power[mask].sum()) / max(spectral_total, 1e-20)

    centroid_mask = (frequencies >= 20.0) & (frequencies <= min(20000.0, sample_rate / 2.0))
    centroid = float(np.sum(frequencies[centroid_mask] * power[centroid_mask]) / max(float(power[centroid_mask].sum()), 1e-20))

    correlation = np.corrcoef(response.T) if channels > 1 else np.ones((1, 1))
    off_diag = np.abs(correlation[np.triu_indices(channels, 1)]) if channels > 1 else np.array([1.0])
    channel_energy = np.sum(response * response, axis=0)
    channel_energy_db = 10.0 * np.log10(np.maximum(channel_energy / max(float(channel_energy.max()), 1e-20), 1e-12))
    onset_spread_ms = (max(channel_onsets) - min(channel_onsets)) * 1000.0 / sample_rate

    peak_frame = int(np.argmax(frame_energy))
    peak_to_rms_db = 20.0 * math.log10(peak / max(float(np.sqrt(np.mean(response * response))), 1e-12))
    c50_db = 10.0 * math.log10(max(early, 1e-12) / max(late, 1e-12))
    edt_ms = decay_time_ms(frame_energy, sample_rate, 0.0, -10.0, 1.0)
    t20_ms = decay_time_ms(frame_energy, sample_rate, -5.0, -25.0, 1.0)
    t30_ms = decay_time_ms(frame_energy, sample_rate, -5.0, -35.0, 1.0)

    tail_pct = 100.0 * late
    if tail_pct >= 8.0 and (t20_ms or 0.0) >= 100.0:
        shape = "diffuse_cabin_candidate"
    elif tail_pct >= 2.0:
        shape = "short_cabin_transfer_candidate"
    else:
        shape = "direct_or_filtered_transfer"

    result: dict[str, object] = {
        "file": path.name,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "sample_rate": sample_rate,
        "channels": channels,
        "frames": frames,
        "duration_ms": frames * 1000.0 / sample_rate,
        "onset_ms": onset * 1000.0 / sample_rate,
        "onset_spread_ms": onset_spread_ms,
        "peak_ms_after_onset": peak_frame * 1000.0 / sample_rate,
        "peak": peak,
        "peak_to_rms_db": peak_to_rms_db,
        "energy_0_5_pct": 100.0 * e0_5,
        "energy_5_20_pct": 100.0 * e5_20,
        "energy_20_50_pct": 100.0 * e20_50,
        "energy_50_100_pct": 100.0 * e50_100,
        "energy_100_plus_pct": 100.0 * e100_plus,
        "c50_db": c50_db,
        "edt_ms": edt_ms,
        "t20_ms": t20_ms,
        "t30_ms": t30_ms,
        "spectral_centroid_hz": centroid,
        "median_abs_channel_correlation": float(np.median(off_diag)),
        "channel_energy_range_db": float(channel_energy_db.max() - channel_energy_db.min()),
        "shape": shape,
    }
    result.update(band_ratios)
    return result


def finite_round(value: object) -> object:
    if isinstance(value, float):
        return round(value, 6) if math.isfinite(value) else None
    return value


def derive_reference(metrics: list[dict[str, object]]) -> dict[str, object]:
    # Four-channel responses have balanced energy in every stored channel. The
    # eight-channel group contains 58-81 dB channel-energy ranges, so its matrix
    # layout is unknown and is not safe as the reference for stereo radio.
    candidates = [m for m in metrics if int(m["channels"]) == 4 and float(m["channel_energy_range_db"]) <= 10.0]
    if not candidates:
        candidates = metrics
    unique = {}
    for metric in candidates:
        unique.setdefault(str(metric["sha256"]), metric)
    candidates = list(unique.values())

    def median(key: str, fallback: float) -> float:
        values = [float(m[key]) for m in candidates if m.get(key) is not None]
        return float(np.median(values)) if values else fallback

    # Values are bounded to the stable range of the real-time model. The wet
    # amount is intentionally conservative because the source remains music,
    # not a dry measurement sweep.
    t20_seconds = np.clip(median("t20_ms", 220.0) / 1000.0, 0.12, 0.45)
    tail_fraction = np.clip(median("energy_20_50_pct", 3.0) + median("energy_50_100_pct", 2.0), 1.0, 20.0) / 100.0
    wet = float(np.clip(0.09 + 0.45 * tail_fraction, 0.10, 0.17))
    return {
        "candidate_count": len(candidates),
        "selected_files": [m["file"] for m in candidates],
        "selection_rule": "unique 4-channel IRs with <=10 dB channel-energy range",
        "profile_decay_seconds": float(t20_seconds),
        "recommended_cabin_wet": wet,
        "recommended_reflection_amount": float(np.clip(0.42 + 1.5 * tail_fraction, 0.45, 0.68)),
        "median_c50_db": median("c50_db", 10.0),
        "median_spectral_centroid_hz": median("spectral_centroid_hz", 3500.0),
        "interpretation_limit": "Shape-only inference; no runtime event-to-IR mapping was available.",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.input_dir.glob("*.wav"))
    if not paths:
        raise SystemExit(f"No WAV files in {args.input_dir}")
    metrics = [{key: finite_round(value) for key, value in analyze(path).items()} for path in paths]
    reference = {key: finite_round(value) for key, value in derive_reference(metrics).items()}
    args.output_dir.mkdir(parents=True, exist_ok=True)

    json_path = args.output_dir / "cockpit_ir_metrics.json"
    json_path.write_text(json.dumps({"files": metrics, "reference": reference}, indent=2), encoding="utf-8")
    csv_path = args.output_dir / "cockpit_ir_metrics.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(metrics[0]))
        writer.writeheader()
        writer.writerows(metrics)
    print(json.dumps({"files": len(metrics), "json": str(json_path), "csv": str(csv_path), "reference": reference}, indent=2))


if __name__ == "__main__":
    main()
