# DISP-TR Reverse Dispersion UI Contract (retired)

Status: retired on 2026-08-09.

DISP-TR no longer exposes or activates Reverse Dispersion. The `reverse`
parameter remains backend-only and defaults to `false` solely so older saved
states can be loaded without changing the forward processor topology. No MAIN
row, prompt, tooltip, Matrix destination or automation promise is permitted.

The remainder of this file is historical design context and is not an active UI
contract.

**Status:** deprecated and excluded from the 1.4 release
**Scope:** UI, host-facing parameter exposure and preset compatibility. The DSP topology remains owned by `ReverseDispersionRuntime`.

## Product meaning

`reverse` is a boolean mode that changes the perceptual direction of the dispersion contour. It is **not** sample-accurate temporal reversal, grain reversal, lookahead, convolution reversal or an added-latency mode.

The public UI label is `REVERSE`. The parameter's host name remains `Reverse Dispersion` so automation lanes and saved states are explicit. The tooltip must explain the distinction above.

## Placement and hierarchy

The control belongs to the `MAIN` page, beside the stereo topology controls in the semantic order:

`STYLE -> REVERSE -> CHAOS FILTER -> CHAOS DELAY`

It is a toggle mode control, not a normal tonal row and not an inspector prompt. It must use the shared toggle presentation, focus keyline, palette roles and tooltip behaviour. No duplicate Reverse control may appear in I/O, prompts, MACROS or sidechain.

## Automation and transitions

`reverse` remains host-automatable because it changes the creative character of the effect. The processor owns transition smoothing/crossfade; the UI must not fake a continuous value or imply a latency transition. Automation must update the toggle state in real time and preserve the normal focus contract.

Forward is the default (`false`). A mode change must not alter PDC: the causal implementation reports no additional reverse latency. Bypass, `MIX = 0`, `AMOUNT = 0` and inactive processing may leave the visual toggle state visible while the audible result is correctly silent or dry.

## Parameter interaction contract

| Parameter | Reverse behaviour |
|---|---|
| FREQ | Rebuilds the reverse contour around the same centre frequency. |
| SHAPE | Changes contour concentration in the reverse bank using the same semantic control. |
| AMOUNT / STAGES | Controls reverse stage depth/count; zero remains neutral. |
| SERIES | Applies to the reverse bank with the same series count. |
| JITTER | Forward semantics remain available; native reverse jitter coverage is limited and must not be advertised as an exact mirror. |
| ALT | Applies the existing alternate polarity rule where supported by the reverse bank. |
| FEEDBACK | Remains in the existing feedback path. Reverse plus feedback can produce longer tails and is not temporal inversion. |
| STYLE | MONO, STEREO, WIDE and DUAL retain their existing meaning. DUAL currently shares the reverse bank; the UI must not imply independent reverse banks. |
| Sidechain | Existing sidechain controls remain available. Reverse does not create a new sidechain panel or promise per-sample reverse-sidechain parity. |
| MACROS | `reverse` is excluded as a destination. Boolean mode automation remains available through the host parameter. |

## Persistence and compatibility

The parameter is included in the shared definition whitelist as a direct musical parameter. Older states without `reverse` use the missing-parameter default `false` supplied by the JUCE parameter layout, preserving forward behaviour. New presets store the boolean value normally.

## Required visual states

The UI probe must cover: forward, reverse, preset restore, host-style parameter change, playback refresh, Amount 0, Mix 0, bypass and the reverse transition. Every state must retain the same control bounds and shared focus treatment; only the toggle state and signature/DSP-derived visual response may change.

## Release limitation

This contract does not authorise a reverse temporal or FIR/lookahead mode. Such a feature would require a separate product mode, explicit PDC policy, new DSP acceptance tests and a different UI label.
