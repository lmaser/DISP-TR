# DISP-TR v1.4

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
- **Collapsible INPUT/OUTPUT/MIX section**: Click the toggle bar (triangle) at the top of the slider area to swap between main parameters and the INPUT, OUTPUT, MIX controls. The toggle bar stays fixed in place; only the arrow direction changes. State persists across sessions and preset changes.
- **Filter bar**: Visible in the INPUT/OUTPUT/MIX section. Click to open the HP/LP filter configuration prompt with frequency, slope, and enable/disable controls for each filter.
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

### FEEDBACK (−100 to +100%)

Feeds the all-pass chain output back into its input, creating resonant peaks. Negative values invert the feedback polarity, producing a different set of resonant frequencies (notch-to-peak inversion).  
Uses sign-preserving bipolar smoothstep mapping for musical control: gentle at low values, increasingly intense toward the extremes.  
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

### HP/LP FILTER

High-pass and low-pass filters applied to the wet signal, accessible via the filter bar in the IO section.

- **HP FREQ (20–20 000 Hz)**: High-pass cutoff frequency.
- **LP FREQ (20–20 000 Hz)**: Low-pass cutoff frequency.
- **HP SLOPE (6 dB / 12 dB / 24 dB)**: High-pass filter slope.
- **LP SLOPE (6 dB / 12 dB / 24 dB)**: Low-pass filter slope.
- **HP / LP toggles**: Enable or disable each filter independently. Click the HP/LP label or its checkbox to toggle.

Slope modes:
- **6 dB/oct**: Single-pole filter.
- **12 dB/oct**: Second-order Butterworth.
- **24 dB/oct**: Two cascaded second-order Butterworth stages.

### INV (Invert)

Inverts output polarity (multiplies signal by −1).

### MD (MIDI)

Enables MIDI note control of the FREQUENCY parameter.  
When active, incoming MIDI note-on messages override the frequency slider with the note's pitch.  
MIDI velocity controls glide speed (higher velocity = faster transitions).  
Channel can be configured via right-click on the MIDI channel display (0 = omni, 1–16 = specific).

### CHAOS

Micro-variation engine that adds organic randomness to the effect. Two independent chaos targets:

- **CHAOS F (Filter)**: Modulates the HP/LP filter cutoff frequencies when filters are enabled. Creates evolving tonal movement.
- **CHAOS D (Disperser)**: Modulates the center FREQUENCY parameter. Produces drifting, alive-sounding dispersion.

Each chaos target has its own toggle and shares two global controls:

- **AMOUNT (0–100%)**: Modulation depth — how far from the base value the parameter can drift. Default: 50%.
- **SPEED (0.01–100 Hz)**: Sample-and-hold rate — how often a new random target is picked. Default: 5 Hz.

Uses exponential smoothing between random targets for glitch-free transitions.

### LIM THRESHOLD (−36 to 0 dB)

Peak limiter threshold. Sets the ceiling above which the limiter engages.
At 0 dB (default) the limiter acts as a transparent safety net. Lower values compress the signal harder.

### LIM MODE

Limiter insertion point:
- **NONE**: Limiter disabled.
- **WET**: Limiter applied to the wet signal only (after processing, before dry/wet mix).
- **GLOBAL**: Limiter applied to the final output (after output gain and dry/wet mix).

The limiter is a dual-stage transparent peak limiter:
- **Stage 1 (Leveler)**: 2 ms attack, 10 ms release — catches sustained overs.
- **Stage 2 (Brickwall)**: Instant attack, 100 ms release — catches transient peaks.

Stereo-linked gain reduction ensures consistent imaging.

## Technical Details

### DSP Architecture
- **All-pass filter**: First-order, `y = coeff * (x − z1) + z1` with per-stage state.
- **Coefficient**: `tan(π * frequency / sampleRate)` mapped through `(1 − c) / (1 + c)`.
- **Stage distribution**: SHAPE fans stage frequencies around FREQUENCY using a power-curve mapping with low-frequency compensation.
- **Feedback**: Sign-preserving bipolar smoothstep-mapped output → input loop with per-channel state. Positive and negative feedback produce distinct resonant characters.
- **Smoothing**: EMA for frequency (80 ms tau), linear SmoothedValue for stages (60 ms), shape (50 ms), and feedback (50 ms).
- **Fast path**: When all parameters are converged and no crossfade is active, a tight inner loop runs without per-sample smoothing or coefficient checks.
- **Series crossfade**: 20 ms linear crossfade between old and new series topology on changes.
- **Chaos**: Sample-and-hold random modulation with exponential smoothing. Per-block coefficient precomputation avoids per-sample `std::exp` calls.
- **MIDI**: Note-to-frequency via `440 * 2^((note-69)/12)`. Velocity-dependent glide via EMA time constant.
- **Wet filter**: Biquad HP/LP on the wet signal. Transposed Direct Form II. Coefficients updated once per block (channel 0), shared across channels.
- **Fast dB→gain**: `std::exp2(x * 0.166)` approximation replacing `std::pow(10, x/20)` for input/output gain conversion.

### State Persistence
- All parameters saved via JUCE AudioProcessorValueTreeState.
- UI state (window size, palette, CRT toggle, custom colours, MIDI channel, IO section expanded/collapsed) persisted separately in the processor's state block.

## Changelog

### v1.4
- Feedback is now bipolar (−100% to +100%). Negative feedback inverts the feedback polarity, producing a different resonant character (notch-to-peak inversion). Uses sign-preserving smoothstep mapping.
- Added CHAOS engine with two independent targets: CHAOS F (filter frequency modulation) and CHAOS D (disperser frequency modulation). Sample-and-hold with exponential smoothing.
- Added HP/LP filter section on the wet signal path with configurable frequency, slope (6/12/24 dB/oct), and per-filter enable/disable.
- Replaced `std::pow(10, x/20)` with `std::exp2(x * 0.166)` for faster dB-to-gain conversion.
- Precomputed chaos smooth coefficients in `prepareToPlay`, eliminating 4× `std::exp` calls per audio block.
- Cached frequency EMA coefficient, eliminating 1× `std::exp` per block in non-MIDI path.
- Removed duplicate chaos smoothing from outer call sites (already handled inside advance functions).
- Checkbox rendering aligned with TR-series style (full fill when ticked).
- All percentage parameters standardized to 1 decimal place across label, slider bar, and numeric entry prompt.
- Added dual-stage transparent peak limiter with LIM THRESHOLD (−36 to 0 dB) and LIM MODE (NONE/WET/GLOBAL). Stereo-linked gain reduction with 2 ms/10 ms leveler + instant/100 ms brickwall stages.
