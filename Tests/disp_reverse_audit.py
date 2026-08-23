"""Offline audit harness for the proposed DISP-TR causal reverse dispersion.

This is deliberately independent from PluginProcessor. It reproduces the
current first-order allpass frequency layout, proves that reversing stage
order is not a reverse operation, and fits a bounded allpass bank to a
complementary group-delay target.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares


def current_frequencies(sample_rate: float, center_hz: float, shape: float, stages: int) -> np.ndarray:
    min_freq = 20.0
    max_freq = min(20_000.0, 0.49 * sample_rate)
    center = np.clip(center_hz, min_freq, max_freq)
    log_pos = np.log2(center / min_freq) / np.log2(max_freq / min_freq)
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
    frequencies = center * 2.0 ** (0.5 * spread_oct * warped)
    return np.clip(frequencies, min_freq, max_freq)


def allpass_response(frequencies: np.ndarray, sample_rate: float, test_hz: np.ndarray) -> np.ndarray:
    omega = 2.0 * np.pi * test_hz / sample_rate
    z_inv = np.exp(-1j * omega)
    response = np.ones_like(z_inv, dtype=np.complex128)
    for frequency in frequencies:
        tangent = np.tan(np.pi * frequency / sample_rate)
        coefficient = (1.0 - tangent) / (1.0 + tangent)
        response *= (z_inv - coefficient) / (1.0 - coefficient * z_inv)
    return response


def second_order_allpass_response(
    frequencies: np.ndarray,
    radii: np.ndarray,
    sample_rate: float,
    test_hz: np.ndarray,
) -> np.ndarray:
    omega = 2.0 * np.pi * test_hz / sample_rate
    z_inv = np.exp(-1j * omega)
    response = np.ones_like(z_inv, dtype=np.complex128)
    for frequency, radius in zip(frequencies, radii):
        theta = 2.0 * np.pi * frequency / sample_rate
        cosine = np.cos(theta)
        numerator = radius * radius - 2.0 * radius * cosine * z_inv + z_inv * z_inv
        denominator = 1.0 - 2.0 * radius * cosine * z_inv + radius * radius * z_inv * z_inv
        response *= numerator / denominator
    return response


def group_delay(response: np.ndarray, test_hz: np.ndarray) -> np.ndarray:
    phase = np.unwrap(np.angle(response))
    return -np.gradient(phase, 2.0 * np.pi * test_hz)


def fit_complementary_bank(
    forward_frequencies: np.ndarray,
    sample_rate: float,
    test_hz: np.ndarray,
) -> tuple[np.ndarray, float, float]:
    forward = allpass_response(forward_frequencies, sample_rate, test_hz)
    forward_delay = group_delay(forward, test_hz)
    delay_constant = float(np.max(forward_delay) + np.percentile(forward_delay, 10.0))
    target = delay_constant - forward_delay
    log_bounds = (np.log2(20.0), np.log2(min(20_000.0, 0.49 * sample_rate)))
    initial = np.linspace(log_bounds[0], log_bounds[1], len(forward_frequencies))

    def residual(log_frequencies: np.ndarray) -> np.ndarray:
        candidate = allpass_response(2.0 ** log_frequencies, sample_rate, test_hz)
        delay = group_delay(candidate, test_hz)
        scale = max(float(np.max(np.abs(target))), 1.0)
        return (delay - target) / scale

    starts = [initial, initial[::-1]]
    rng = np.random.default_rng(0x5452)
    starts.extend(np.sort(rng.uniform(log_bounds[0], log_bounds[1], len(initial))) for _ in range(8))
    best_result = None
    best_cost = float("inf")
    for start in starts:
        result = least_squares(
            residual,
            start,
            bounds=log_bounds,
            max_nfev=220,
            loss="soft_l1",
            xtol=1.0e-10,
            ftol=1.0e-10,
            gtol=1.0e-10,
        )
        cost = float(np.mean(residual(result.x) ** 2))
        if cost < best_cost:
            best_cost = cost
            best_result = result
    assert best_result is not None
    result = best_result
    candidate_frequencies = np.sort(2.0 ** result.x)
    rmse_samples = float(np.sqrt(np.mean((residual(result.x) * max(np.max(np.abs(target)), 1.0)) ** 2)))
    return candidate_frequencies, delay_constant, rmse_samples


def render_impulse(frequencies: np.ndarray, sample_rate: float, length: int) -> np.ndarray:
    output = np.zeros(length, dtype=np.float64)
    states = np.zeros(len(frequencies), dtype=np.float64)
    coefficients = []
    for frequency in frequencies:
        tangent = np.tan(np.pi * frequency / sample_rate)
        coefficients.append((1.0 - tangent) / (1.0 + tangent))
    for index in range(length):
        value = 1.0 if index == 0 else 0.0
        for stage, coefficient in enumerate(coefficients):
            result = -coefficient * value + states[stage]
            states[stage] = value + coefficient * result
            value = result
        output[index] = value
    return output


def render_second_order_impulse(
    frequencies: np.ndarray,
    radii: np.ndarray,
    sample_rate: float,
    length: int,
    block_size: int,
) -> np.ndarray:
    output = np.zeros(length, dtype=np.float64)
    x1 = np.zeros(len(frequencies), dtype=np.float64)
    x2 = np.zeros(len(frequencies), dtype=np.float64)
    y1 = np.zeros(len(frequencies), dtype=np.float64)
    y2 = np.zeros(len(frequencies), dtype=np.float64)
    coefficients = []
    for frequency, radius in zip(frequencies, radii):
        theta = 2.0 * np.pi * frequency / sample_rate
        cosine = np.cos(theta)
        coefficients.append((radius * radius, -2.0 * radius * cosine, 1.0, -2.0 * radius * cosine, radius * radius))

    for block_start in range(0, length, block_size):
        block_end = min(length, block_start + block_size)
        for index in range(block_start, block_end):
            value = 1.0 if index == 0 else 0.0
            for stage, (b0, b1, b2, a1, a2) in enumerate(coefficients):
                result = b0 * value + b1 * x1[stage] + b2 * x2[stage] - a1 * y1[stage] - a2 * y2[stage]
                x2[stage] = x1[stage]
                x1[stage] = value
                y2[stage] = y1[stage]
                y1[stage] = result
                value = result
            output[index] = value
    return output


def render_variant_impulse(
    frequencies: np.ndarray,
    radii: np.ndarray,
    sample_rate: float,
    length: int,
    block_size: int,
    series: int = 1,
    feedback: float = 0.0,
    jitter_depth: float = 0.0,
    alt: bool = False,
    modulation_depth: float = 0.0,
) -> np.ndarray:
    stage_count = len(frequencies) * series
    x1 = np.zeros(stage_count, dtype=np.float64)
    x2 = np.zeros(stage_count, dtype=np.float64)
    y1 = np.zeros(stage_count, dtype=np.float64)
    y2 = np.zeros(stage_count, dtype=np.float64)
    output = np.zeros(length, dtype=np.float64)
    previous_output = 0.0

    for block_start in range(0, length, block_size):
        block_end = min(length, block_start + block_size)
        phase = 0.37 * (block_start // max(block_size, 1))
        jitter_scale = 1.0 + jitter_depth * 0.01 * np.sin(phase)
        coefficients = []
        for _ in range(series):
            for stage_index, (frequency, radius) in enumerate(zip(frequencies, radii)):
                alt_scale = 1.0 + (0.02 if stage_index % 2 == 0 else -0.02) if alt else 1.0
                modulation = 1.0 + modulation_depth * 0.03 * np.sin(phase + stage_index * 0.17)
                theta = 2.0 * np.pi * frequency * jitter_scale * alt_scale * modulation / sample_rate
                cosine = np.cos(theta)
                coefficients.append((radius * radius, -2.0 * radius * cosine, 1.0,
                                     -2.0 * radius * cosine, radius * radius))
        for index in range(block_start, block_end):
            value = (1.0 if index == 0 else 0.0) + feedback * previous_output
            for stage, (b0, b1, b2, a1, a2) in enumerate(coefficients):
                result = b0 * value + b1 * x1[stage] + b2 * x2[stage] - a1 * y1[stage] - a2 * y2[stage]
                x2[stage] = x1[stage]
                x1[stage] = value
                y2[stage] = y1[stage]
                y1[stage] = result
                value = result
            previous_output = value
            output[index] = value
    return output


def tail_samples(signal: np.ndarray, relative_db: float = -80.0) -> int:
    peak = float(np.max(np.abs(signal)))
    if peak <= 0.0:
        return 0
    threshold = peak * 10.0 ** (relative_db / 20.0)
    active = np.flatnonzero(np.abs(signal) >= threshold)
    return int(active[-1] + 1) if len(active) else 0


def one_pole_filter(signal: np.ndarray, sample_rate: float, cutoff_hz: float, highpass: bool) -> np.ndarray:
    coefficient = np.exp(-2.0 * np.pi * cutoff_hz / sample_rate)
    state = 0.0
    output = np.zeros_like(signal)
    for index, sample in enumerate(signal):
        state = (1.0 - coefficient) * sample + coefficient * state
        output[index] = sample - state if highpass else state
    return output


def crossfade_parameter_change(old_signal: np.ndarray, new_signal: np.ndarray, change_at: int, fade_samples: int) -> np.ndarray:
    output = old_signal.copy()
    end = min(len(output), change_at + fade_samples)
    if end > change_at:
        ramp = np.linspace(0.0, 1.0, end - change_at, endpoint=False)
        output[change_at:end] = old_signal[change_at:end] * (1.0 - ramp) + new_signal[change_at:end] * ramp
    if end < len(output):
        output[end:] = new_signal[end:]
    return output


def band_mean(values: np.ndarray, frequencies: np.ndarray, low_hz: float, high_hz: float) -> float:
    mask = (frequencies >= low_hz) & (frequencies <= high_hz)
    return float(np.mean(values[mask]))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sample-rate", type=float, default=48_000.0)
    parser.add_argument("--center", type=float, default=1_000.0)
    parser.add_argument("--shape", type=float, default=0.65)
    parser.add_argument("--stages", type=int, default=16)
    parser.add_argument("--output", type=Path, default=Path("analysis_out/disp_reverse_audit.json"))
    args = parser.parse_args()

    test_hz = np.geomspace(20.0, min(20_000.0, 0.49 * args.sample_rate), 1024)
    frequencies = current_frequencies(args.sample_rate, args.center, args.shape, args.stages)
    forward = allpass_response(frequencies, args.sample_rate, test_hz)
    reversed_order = allpass_response(frequencies[::-1], args.sample_rate, test_hz)
    candidate, delay_constant, delay_rmse = fit_complementary_bank(
        frequencies, args.sample_rate, test_hz
    )
    candidate_response = allpass_response(candidate, args.sample_rate, test_hz)
    mirrored_frequencies = np.clip(
        0.5 * args.sample_rate - frequencies,
        20.0,
        min(20_000.0, 0.49 * args.sample_rate),
    )
    mirrored_response = allpass_response(mirrored_frequencies, args.sample_rate, test_hz)
    second_order_frequencies = np.geomspace(
        max(8_000.0, min(12_000.0, 0.45 * args.sample_rate)),
        min(20_000.0, 0.49 * args.sample_rate),
        max(4, args.stages // 2),
    )
    second_order_radii = np.full_like(second_order_frequencies, 0.9)
    second_order_response = second_order_allpass_response(
        second_order_frequencies, second_order_radii, args.sample_rate, test_hz
    )
    parameterized_reverse_frequencies = np.clip(
        min(20_000.0, 0.49 * args.sample_rate) - frequencies[::2],
        1_000.0,
        min(20_000.0, 0.49 * args.sample_rate) - 20.0,
    )
    parameterized_reverse_radii = np.full(
        len(parameterized_reverse_frequencies), 0.84 + 0.10 * args.shape
    )
    parameterized_reverse_response = second_order_allpass_response(
        parameterized_reverse_frequencies,
        parameterized_reverse_radii,
        args.sample_rate,
        test_hz,
    )
    forward_delay = group_delay(forward, test_hz)
    candidate_delay = group_delay(candidate_response, test_hz)
    mirrored_delay = group_delay(mirrored_response, test_hz)
    second_order_delay = group_delay(second_order_response, test_hz)
    parameterized_reverse_delay = group_delay(parameterized_reverse_response, test_hz)
    target_delay = delay_constant - forward_delay
    impulse = render_impulse(candidate, args.sample_rate, 4096)
    streaming_reference = render_second_order_impulse(
        parameterized_reverse_frequencies,
        parameterized_reverse_radii,
        args.sample_rate,
        4096,
        4096,
    )
    streaming_deltas = []
    for block_size in (16, 32, 64, 128, 256, 512, 1024):
        rendered = render_second_order_impulse(
            parameterized_reverse_frequencies,
            parameterized_reverse_radii,
            args.sample_rate,
            4096,
            block_size,
        )
        streaming_deltas.append(float(np.max(np.abs(rendered - streaming_reference))))
    variant_results = {}
    for series in (1, 2):
        for feedback in (0.0, 0.25, 0.5, 0.75):
            key = f"series_{series}_feedback_{feedback:.2f}"
            signal = render_variant_impulse(
                parameterized_reverse_frequencies,
                parameterized_reverse_radii,
                args.sample_rate,
                8192,
                128,
                series=series,
                feedback=feedback,
            )
            variant_results[key] = {
                "max_abs": float(np.max(np.abs(signal))),
                "tail_samples_at_minus_80_db": tail_samples(signal),
                "finite": bool(np.all(np.isfinite(signal))),
            }
    jitter_signal = render_variant_impulse(
        parameterized_reverse_frequencies,
        parameterized_reverse_radii,
        args.sample_rate,
        8192,
        128,
        series=2,
        feedback=0.5,
        jitter_depth=1.0,
    )
    alt_mod_signal = render_variant_impulse(
        parameterized_reverse_frequencies,
        parameterized_reverse_radii,
        args.sample_rate,
        8192,
        128,
        series=2,
        feedback=0.5,
        jitter_depth=1.0,
        alt=True,
        modulation_depth=1.0,
    )
    filtered_signal = one_pole_filter(
        alt_mod_signal, args.sample_rate, 18_000.0, highpass=False
    )
    filtered_signal = one_pole_filter(
        filtered_signal, args.sample_rate, 20.0, highpass=True
    )
    old_parameters = render_variant_impulse(
        parameterized_reverse_frequencies,
        parameterized_reverse_radii,
        args.sample_rate,
        8192,
        128,
        series=2,
        feedback=0.5,
    )
    new_frequencies = np.clip(parameterized_reverse_frequencies * 0.72, 1_000.0, min(20_000.0, 0.49 * args.sample_rate) - 20.0)
    new_parameters = render_variant_impulse(
        new_frequencies,
        parameterized_reverse_radii,
        args.sample_rate,
        8192,
        128,
        series=2,
        feedback=0.5,
    )
    automated_signal = crossfade_parameter_change(old_parameters, new_parameters, 2048, 256)
    transition_window = automated_signal[2040:2304]
    transition_jump = float(np.max(np.abs(np.diff(transition_window))))
    surrounding_jump = float(np.percentile(np.abs(np.diff(old_parameters)), 99.0))
    candidate_delay_error = np.abs(candidate_delay - target_delay)
    low_band = (20.0, 200.0)
    high_band = (12_000.0, min(20_000.0, 0.49 * args.sample_rate))
    forward_low_delay = band_mean(forward_delay, test_hz, *low_band)
    forward_high_delay = band_mean(forward_delay, test_hz, *high_band)
    mirrored_low_delay = band_mean(mirrored_delay, test_hz, *low_band)
    mirrored_high_delay = band_mean(mirrored_delay, test_hz, *high_band)
    second_order_low_delay = band_mean(second_order_delay, test_hz, *low_band)
    second_order_high_delay = band_mean(second_order_delay, test_hz, *high_band)
    parameterized_reverse_low_delay = band_mean(parameterized_reverse_delay, test_hz, *low_band)
    parameterized_reverse_high_delay = band_mean(parameterized_reverse_delay, test_hz, *high_band)

    report = {
        "sample_rate": args.sample_rate,
        "center_hz": args.center,
        "shape": args.shape,
        "stages": args.stages,
        "forward_frequencies_hz": frequencies.tolist(),
        "candidate_reverse_frequencies_hz": candidate.tolist(),
        "mirrored_reverse_frequencies_hz": mirrored_frequencies.tolist(),
        "second_order_reverse_frequencies_hz": second_order_frequencies.tolist(),
        "second_order_reverse_radius": 0.9,
        "parameterized_reverse_frequencies_hz": parameterized_reverse_frequencies.tolist(),
        "parameterized_reverse_radius": float(parameterized_reverse_radii[0]),
        "stage_order_equivalence_max": float(np.max(np.abs(forward - reversed_order))),
        "forward_magnitude_error_db_max": float(np.max(np.abs(20.0 * np.log10(np.maximum(np.abs(forward), 1.0e-15))))),
        "candidate_magnitude_error_db_max": float(np.max(np.abs(20.0 * np.log10(np.maximum(np.abs(candidate_response), 1.0e-15))))),
        "target_delay_constant_samples": delay_constant * args.sample_rate,
        "forward_delay_samples_min": float(np.min(forward_delay) * args.sample_rate),
        "forward_delay_samples_max": float(np.max(forward_delay) * args.sample_rate),
        "target_reverse_delay_samples_min": float(np.min(target_delay) * args.sample_rate),
        "candidate_delay_rmse_samples": delay_rmse * args.sample_rate,
        "candidate_delay_error_p95_samples": float(np.percentile(candidate_delay_error, 95.0) * args.sample_rate),
        "candidate_delay_error_max_samples": float(np.max(candidate_delay_error) * args.sample_rate),
        "candidate_delay_samples_min": float(np.min(candidate_delay) * args.sample_rate),
        "candidate_delay_samples_max": float(np.max(candidate_delay) * args.sample_rate),
        "forward_low_band_delay_samples": forward_low_delay * args.sample_rate,
        "forward_high_band_delay_samples": forward_high_delay * args.sample_rate,
        "mirrored_low_band_delay_samples": mirrored_low_delay * args.sample_rate,
        "mirrored_high_band_delay_samples": mirrored_high_delay * args.sample_rate,
        "second_order_low_band_delay_samples": second_order_low_delay * args.sample_rate,
        "second_order_high_band_delay_samples": second_order_high_delay * args.sample_rate,
        "parameterized_reverse_low_band_delay_samples": parameterized_reverse_low_delay * args.sample_rate,
        "parameterized_reverse_high_band_delay_samples": parameterized_reverse_high_delay * args.sample_rate,
        "candidate_impulse_peak_index": int(np.argmax(np.abs(impulse))),
        "streaming_block_max_deltas": streaming_deltas,
        "variant_results": variant_results,
        "jitter_variant_max_abs": float(np.max(np.abs(jitter_signal))),
        "jitter_variant_tail_samples_at_minus_80_db": tail_samples(jitter_signal),
        "jitter_variant_finite": bool(np.all(np.isfinite(jitter_signal))),
        "alt_mod_filter_max_abs": float(np.max(np.abs(filtered_signal))),
        "alt_mod_filter_tail_samples_at_minus_80_db": tail_samples(filtered_signal),
        "automation_transition_max_jump": transition_jump,
        "automation_surrounding_p99_jump": surrounding_jump,
        "tests": {
            "stage_order_is_not_reverse": bool(np.max(np.abs(forward - reversed_order)) < 1.0e-8),
            "forward_allpass_magnitude": bool(np.max(np.abs(np.abs(forward) - 1.0)) < 1.0e-8),
            "candidate_allpass_magnitude": bool(np.max(np.abs(np.abs(candidate_response) - 1.0)) < 1.0e-8),
            "candidate_delay_is_causal": bool(np.min(candidate_delay) >= -1.0e-8),
            "candidate_delay_fit_is_release_ready": bool(
                delay_rmse * args.sample_rate <= 2.0
                and np.percentile(candidate_delay_error, 95.0) * args.sample_rate <= 8.0
            ),
            "mirrored_contour_is_reversed": bool(
                forward_low_delay > forward_high_delay * 4.0
                and mirrored_high_delay > mirrored_low_delay * 4.0
            ),
            "mirrored_allpass_magnitude": bool(np.max(np.abs(np.abs(mirrored_response) - 1.0)) < 1.0e-8),
            "second_order_contour_is_reversed": bool(
                second_order_high_delay > second_order_low_delay * 4.0
            ),
            "second_order_allpass_magnitude": bool(
                np.max(np.abs(np.abs(second_order_response) - 1.0)) < 1.0e-8
            ),
            "second_order_sections_are_stable": bool(np.all(second_order_radii < 1.0)),
            "parameterized_contour_is_reversed": bool(
                parameterized_reverse_high_delay > parameterized_reverse_low_delay * 4.0
            ),
            "parameterized_allpass_magnitude": bool(
                np.max(np.abs(np.abs(parameterized_reverse_response) - 1.0)) < 1.0e-8
            ),
            "parameterized_sections_are_stable": bool(
                np.all(parameterized_reverse_radii < 1.0)
            ),
            "streaming_is_block_invariant": bool(max(streaming_deltas) < 1.0e-12),
            "series_feedback_variants_are_finite": bool(
                all(result["finite"] for result in variant_results.values())
            ),
            "series_feedback_variants_are_bounded": bool(
                all(result["max_abs"] < 100.0 for result in variant_results.values())
            ),
            "jitter_variant_is_finite_and_bounded": bool(
                np.isfinite(jitter_signal).all() and np.max(np.abs(jitter_signal)) < 100.0
            ),
            "alt_mod_filter_variant_is_finite_and_bounded": bool(
                np.isfinite(filtered_signal).all() and np.max(np.abs(filtered_signal)) < 100.0
            ),
            "automation_crossfade_is_finite": bool(np.isfinite(automated_signal).all()),
            "automation_crossfade_has_no_large_spike": bool(
                transition_jump < max(0.1, surrounding_jump * 8.0)
            ),
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
