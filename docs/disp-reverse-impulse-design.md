# DISP-TR Reverse Impulse Design

## Decision

`Reverse Dispersion` must mean a causal approximation of the time-reversed
forward impulse response. The existing second-order allpass bank is rejected for
this purpose: it reverses part of a group-delay contour but it does not reverse
the impulse response.

## Why the current bank cannot do it

For a forward response `h[n]`, the requested response is approximately

```text
hReverse[n] = hForward[L - 1 - n]
```

For an allpass IIR, the exact reversed response is generally anticausal (or has
an unstable causal realization). A causal implementation therefore needs a
fixed delay and an approximation. Changing stage order or replacing each stage
frequency with `maxFrequency - frequency` changes phase/group delay, but does
not produce the reversed samples.

## Proposed architecture

1. Build a deterministic forward reference for the current structural state.
2. Capture a bounded impulse response until its energy is below the selected
   tail threshold, with a hard maximum length.
3. Reverse the bounded response, add a fixed causal delay, and window/regularise
   it to produce a finite FIR.
4. Convolve with a partitioned OLA/OLS engine. The first partition defines the
   reported latency; no audio-thread allocation or IR construction is allowed.
5. Build the next IR on a worker/control path, publish it atomically, and
   crossfade the old/new convolution state.

## Initial release limits

- Reference sample rates: 44.1, 48 and 96 kHz.
- Reverse FIR length and partition size must be measured, not guessed.
- `Reverse + Feedback` is initially excluded from the exact-IR claim because
  feedback reverses the complete recursive network, not only the dry allpass.
- Jitter and audio-rate sidechain modulation must not silently rebuild an IR for
  every sample. They require a documented structural-rate policy or remain on
  the forward path until qualified.
- `Series` and `DUAL` require independent reference responses and tests.

## Acceptance criteria

- Direct/reverse impulse mirror correlation and error are reported at 44.1/48/96
  kHz, with a release threshold fixed before implementation.
- Reverse magnitude response remains within the defined FIR approximation error.
- No clicks at activation, bypass, parameter changes or IR swaps.
- Block-size invariance for 32, 64, 128, 256 and 512 samples.
- Reported PDC equals the actual reverse latency at every supported sample rate.
- No allocations, locks or synchronous IR construction in `processBlock`.
- CPU, memory and tail limits are documented for real-time use.

The current reverse route remains disabled for release qualification until this
contract is implemented and the direct/reverse impulse test passes.
