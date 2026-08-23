# DISP-TR Phase 5 Validation

Date: 2026-08-09

## Scope

This pass covers the complete `PluginProcessor` path for the current DISP-TR
implementation, including the Reverse Dispersion route added in Phase 4 and
the shared MACROS/Sidechain activation contract.

The CPU figures below are wall-clock measurements from the Release probe on
the development machine. They are expressed as a percentage of available real
time at 48 kHz, not as a DAW profiler replacement.

## CPU benchmark

The probe renders 480,000 stereo samples with 64 stages, four series, feedback,
and finite-output checks. It includes a warm-up pass and measures only the
processing loop.

| Block | Forward | Reverse | Reverse + jitter + DUAL |
|---:|---:|---:|---:|
| 64 | 2.96% | 5.14% | 9.38% |
| 128 | 2.86% | 4.95% | 9.41% |
| 512 | 2.71% | 4.75% | 9.20% |

The result has substantial real-time margin. The expected trend with larger
blocks is present, and the Reverse path does not show a block-size cliff.

## Automated regression

Passed:

- Reverse plugin probe at 44.1, 48 and 96 kHz.
- Reverse and Reverse+jitter block invariance at 128 and 512 samples.
- Reverse preset round-trip.
- Reverse state/crossfade probe and finite-output checks.
- UI V2 definition: 59 APVTS parameters, 3 musical-state values, 9 retired UI IDs.
- UI family render at 100, 125 and 150 DPI, including prompts and palettes.
- DISP modulation smoke probe, including legacy compatibility and shared modulation.
- Offline reverse DSP audit: stable sections, bounded variants, jitter, automation
  crossfade and no large transition spike.

The probe link still reports three existing JUCE `vorbisfile.c` C4703 warnings;
they originate in the JUCE codec source and are not DISP-TR diagnostics.

## Audio material

The workspace contains deterministic technical material: impulses, sweeps,
sine, step, white/pink noise, bursts and asymmetric stereo signals. These
tests passed finite, bounded and invariance checks.

No trusted musical recording was available in the workspace for a genuine
level-matched listening or blind comparison. Therefore the following release
items remain open:

- audition with drums, bass, pads and full-mix material;
- blind level-matched comparison of Forward/Reverse/DUAL/jitter;
- DAW-host CPU/PDC measurement with the built plugin;
- confirm subjective artefacts during automation and long feedback tails.

## Status

The implementation is not release-complete. The original probe was insufficient:
it only checked that Reverse changed the signal, stayed finite and was block-size
invariant. It did not compare the direct and reverse impulse responses. A stricter
probe run on 2026-08-09 measured, at 48 kHz, direct energy centroid 29.85 samples,
reverse centroid 16.00 samples and direct/reverse time-mirror correlation below
`3e-38`. This is not a reversed impulse response. That measurement describes
the superseded second-order allpass route and is not evidence against the
current FIR prototype.

The same audit also found a UI contract issue: DISP's DSP parameter domain for
Series is 1..4 while the shared choice control assumed zero-based values, so the
default raw value 1 was painted as `2X`. The UI definition now supplies explicit
raw choice values `{1, 2, 3, 4}`. This does not change the DSP default, which was
already `Series=1`.

## FIR integration checkpoint (2026-08-09)

A fixed-size causal FIR A/B runtime is now wired into the production source.
An independent stereo ring-index bug and an insufficient `512`-tap cap were
corrected. The strict plugin probe passes with correlation `1.0` at 44.1, 48
and 96 kHz, with effective tap counts 350, 380 and 740 respectively.

Parameter-triggered IR rebuilds now run on a worker and publish an immutable
bank through an atomic shared-pointer handoff; the audio thread only adopts the
bank and crossfades it. The first configuration remains synchronous to avoid a
silent first block. Audio processing is still sample-by-sample (`1024` maximum
taps), so a partitioned-convolution/CPU decision is still required before
deployment.

## CPU/partition decision (2026-08-09)

The real `PluginProcessor` probe now measures the current worst-case reverse
configuration at approximately `7.85%` real-time for 64-sample blocks,
`7.66%` for 128 samples and `7.56%` for 512 samples at 48 kHz. The same run
passes the impulse correlation at 44.1, 48 and 96 kHz. This is within the
current DISP budget and does not justify introducing FFT latency yet.

The partitioned-convolution option is therefore deferred rather than silently
added: it becomes necessary only if broader host profiling, musical material
or higher-stage/oversampled configurations exceed the agreed CPU budget. The
current implementation still needs final DAW profiling before release.
