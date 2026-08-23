"""Offline reference for a causal FIR approximation of DISP reverse.

This intentionally models only the direct allpass bank. It does not claim to
model feedback, jitter or sidechain modulation; those require separate IR
policies before production integration.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np


def allpass_coefficient(frequency: float, sample_rate: float) -> float:
    tangent = math.tan(math.pi * frequency / sample_rate)
    return (1.0 - tangent) / (1.0 + tangent) if math.isfinite(tangent) else 0.0


def direct_frequencies(sample_rate: float, center: float, shape: float, stages: int) -> np.ndarray:
    minimum = 20.0
    maximum = min(20_000.0, 0.49 * sample_rate)
    center = np.clip(center, minimum, maximum)
    log_pos = math.log2(center / minimum) / math.log2(maximum / minimum)
    low_comp = np.clip(1.0 - log_pos, 0.0, 1.0) ** 1.15
    shape_strength = 1.0 + 0.95 * low_comp
    shape_comp = np.clip(0.5 + (shape - 0.5) * shape_strength, 0.0, 1.0)
    spread_max = 4.0 + 1.1 * low_comp
    spread_oct = spread_max + (0.12 - spread_max) * shape_comp
    warp_gamma = 0.45 + (3.0 + 0.8 * low_comp - 0.45) * shape_comp
    if stages == 1:
        return np.array([center])
    u = np.linspace(-1.0, 1.0, stages)
    warped = np.sign(u) * np.abs(u) ** warp_gamma
    frequencies = center * np.power(2.0, 0.5 * spread_oct * warped)
    return np.clip(frequencies, minimum, maximum)


def render_direct(sample_rate: float, length: int, center: float, shape: float, stages: int, series: int) -> np.ndarray:
    output = np.zeros(length, dtype=np.float64)
    output[0] = 1.0
    for _ in range(series):
        for frequency in direct_frequencies(sample_rate, center, shape, stages):
            coefficient = allpass_coefficient(float(frequency), sample_rate)
            z1 = 0.0
            for index, value in enumerate(output):
                result = -coefficient * value + z1
                z1 = value + coefficient * result
                output[index] = result
    return output


def tail_length(signal: np.ndarray, threshold_db: float) -> int:
    threshold = np.max(np.abs(signal)) * 10.0 ** (threshold_db / 20.0)
    active = np.flatnonzero(np.abs(signal) >= threshold)
    return int(active[-1] + 1) if active.size else 1


def analyse(sample_rate: float, args: argparse.Namespace) -> dict:
    direct = render_direct(sample_rate, args.max_samples, args.center, args.shape,
                           args.stages, args.series)
    results = []
    for threshold_db in (-60.0, -80.0, -100.0):
        length = min(args.max_samples, tail_length(direct, threshold_db))
        bounded_direct = direct[:length].copy()
        # A short tail fade removes the hard truncation discontinuity while
        # keeping most of the direct response available for comparison.
        fade = min(args.fade_samples, length // 4)
        if fade > 0:
            bounded_direct[-fade:] *= np.linspace(1.0, 0.0, fade)
        reversed_fir = bounded_direct[::-1]
        reconstructed = reversed_fir[::-1]
        bounded_reference = np.zeros_like(direct)
        bounded_reference[:length] = reconstructed
        error = bounded_reference - direct
        direct_energy = float(np.dot(direct, direct))
        error_energy = float(np.dot(error, error))
        results.append({
            "threshold_db": threshold_db,
            "fir_length_samples": length,
            "latency_at_48k_ms": 1000.0 * length / sample_rate,
            "mirror_correlation": float(np.dot(reversed_fir, bounded_direct[::-1])
                                          / max(1.0e-30, np.linalg.norm(reversed_fir)
                                               * np.linalg.norm(direct[:length]))),
            "truncation_error_rms_db": 10.0 * math.log10(max(1.0e-30, error_energy)
                                       / max(1.0e-30, direct_energy)),
            "direct_peak": float(np.max(np.abs(direct[:length]))),
            "reverse_peak": float(np.max(np.abs(reversed_fir))),
        })
    return {"sample_rate": sample_rate, "stages": args.stages, "series": args.series,
            "center_hz": args.center, "shape": args.shape, "results": results}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("analysis_out/disp_reverse_fir_prototype.json"))
    parser.add_argument("--max-samples", type=int, default=131072)
    parser.add_argument("--fade-samples", type=int, default=64)
    parser.add_argument("--center", type=float, default=1000.0)
    parser.add_argument("--shape", type=float, default=0.65)
    parser.add_argument("--stages", type=int, default=16)
    parser.add_argument("--series", type=int, default=1)
    args = parser.parse_args()
    report = {"sample_rates": [analyse(sr, args) for sr in (44_100.0, 48_000.0, 96_000.0)]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    for item in report["sample_rates"]:
        print(f"sr={int(item['sample_rate'])} "
              + " ".join(f"{r['threshold_db']:.0f}dB:L={r['fir_length_samples']} "
                         f"mirror={r['mirror_correlation']:.6f} err={r['truncation_error_rms_db']:.2f}dB"
                         for r in item["results"]))


if __name__ == "__main__":
    main()
