# DISP-TR v1.2

<br/><br/>

<img width="446" height="672" alt="image" src="https://github.com/user-attachments/assets/a162fb48-0e8a-4d76-b9ad-70e4f7a08289" />


<br/><br/>

DISP-TR is a phase-dispersion audio effect built on cascaded all-pass filters.  
It reshapes transients and phase response without classic EQ-style magnitude boosts/cuts, creating tighter, softer, or more smeared attack behavior depending on settings.

## Concept

DISP-TR treats phase rotation as the primary creative tool. By stacking all-pass stages in series chains with adjustable frequency distribution, it produces effects ranging from subtle transient reshaping to extreme spectral smearing — all without altering the magnitude spectrum.

## Interface

DISP-TR uses a text-based UI with horizontal bar sliders. All controls are visible at once — no pages, tabs, or hidden menus.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: INV (invert), MD (MIDI). Click to enable/disable.
- **Collapsible INPUT/OUTPUT/MIX section**: Click the toggle bar (triangle) at the top of the slider area to expand or collapse the INPUT, OUTPUT and MIX controls. The expanded/collapsed state persists across sessions and preset changes.
- **Gear icon** (top-right): Opens the info popup with version, credits, and a link to Graphics settings.
- **Graphics popup**: Toggle CRT post-processing effect and switch between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

The value column to the right of each slider shows the current state in context:
- FREQUENCY shows Hz (or MIDI note name when MIDI is active).
- MOD shows the frequency multiplier.
- FEEDBACK shows percentage.
- STAGES shows the number of active all-pass stages.
- SERIES shows the number of cascaded chains.
- SHAPE shows the distribution percentage.
- STYLE shows the stereo mode (MONO, STEREO, WIDE, or DUAL).
- MIX shows dry/wet percentage.

## Parameters

### FREQUENCY (20–20 000 Hz)

Main frequency focus for phase redistribution.  
Low values push the dispersion character toward the low end; high values move it upward.  
Smoothed via EMA (80 ms time constant). When MIDI is active, frequency is overridden by the incoming MIDI note.

### MOD (0.25x–4.0x)

Frequency multiplier. The slider range 0–1 maps non-linearly:
- Left half (0–0.5): 0.25x–1.0x (sub-octave detuning)
- Right half (0.5–1.0): 1.0x–4.0x (harmonic multiplication)

Center position (0.5) = 1.0x (no modification).

### FEEDBACK (0–100%)

Feeds the all-pass chain output back into its input, creating resonant peaks.  
Uses smoothstep mapping (3x²−2x³) for musical control: gentle at low values, increasingly intense toward the top.  
Smoothed linearly (50 ms time constant).

### STAGES (0–128)

Number of all-pass stages used in each series chain.  
Higher values increase phase complexity and effect intensity.  
Smoothed linearly (60 ms time constant) for artifact-free transitions during automation.

### SERIES (1–4)

Number of cascaded chains.  
Each chain is a full copy of the stage network. Higher values deepen the overall dispersion behavior.  
Series changes use a 20 ms crossfade to avoid clicks.

### SHAPE (0–100%)

Controls how spread or warped the per-stage frequency distribution is around FREQUENCY.  
At 0% all stages share the same coefficient. Higher values fan the stages out across the spectrum.  
Smoothed linearly (50 ms time constant).

### STYLE (MONO / STEREO / WIDE / DUAL)

Controls the stereo processing mode:
- **MONO**: Only the left channel is processed; the result is copied to the right channel.
- **STEREO** (default): Both channels are processed independently with identical coefficients.
- **WIDE**: Complementary dispersion — R channel uses negated allpass coefficients (−a), giving an opposite group-delay profile while remaining allpass (flat magnitude, stable). Cross-feedback between channels creates a dimension-expansion effect.
- **DUAL**: R channel processes at half the center frequency (×0.5) with its own coefficient set. Independent feedback per channel — no cross-feed. Produces two distinct dispersion characters, one per side.

### MIX (0–100%)

Dry/wet blend. At 100% the output is fully processed (wet). At 0% the signal passes through unaffected (dry).  
Default is 100%.

### INPUT (−100 to 0 dB)

Pre-processing gain. Controls how much signal enters the all-pass chain.  
Applied to the wet signal only — the dry signal is unaffected.

### OUTPUT (−100 to +24 dB)

Post-processing gain. Applied to the wet signal only.

### INV (Invert)

Inverts output polarity (multiplies signal by −1).

### MD (MIDI)

Enables MIDI note control of the FREQUENCY parameter.  
When active, incoming MIDI note-on messages override the frequency slider with the note's pitch.  
MIDI velocity controls glide speed (higher velocity = faster transitions).  
Channel can be configured via right-click on the MIDI channel display (0 = omni, 1–16 = specific).

## Technical Details

### DSP Architecture
- **All-pass filter**: First-order, `y = coeff * (x − z1) + z1` with per-stage state.
- **Coefficient**: `tan(π * frequency / sampleRate)` mapped through `(1 − c) / (1 + c)`.
- **Stage distribution**: SHAPE fans stage frequencies around FREQUENCY using a power-curve mapping with low-frequency compensation.
- **Feedback**: Smoothstep-mapped (3x²−2x³) output → input loop with per-channel state.
- **Smoothing**: EMA for frequency (80 ms tau), linear SmoothedValue for stages (60 ms), shape (50 ms), and feedback (50 ms).
- **Fast path**: When all parameters are converged and no crossfade is active, a tight inner loop runs without per-sample smoothing or coefficient checks.
- **Series crossfade**: 20 ms linear crossfade between old and new series topology on changes.
- **MIDI**: Note-to-frequency via `440 * 2^((note-69)/12)`. Velocity-dependent glide via EMA time constant.

### State Persistence
- All parameters saved via JUCE AudioProcessorValueTreeState.
- UI state (window size, palette, CRT toggle, custom colours, MIDI channel, IO section expanded/collapsed) persisted separately in the processor's state block.
