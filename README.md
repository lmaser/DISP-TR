# DISP-TR v1.4

<br/><br/>

<img width="446" height="672" alt="image" src="https://github.com/user-attachments/assets/a162fb48-0e8a-4d76-b9ad-70e4f7a08289" />

<br/><br/>

DISP-TR is a phase-dispersion audio effect built on cascaded all-pass filters.
It reshapes transients and phase relationships without classic EQ-style magnitude boosts or cuts, so the sound can become tighter, softer, wider, or more smeared while staying recognizably tied to the source.

## Concept

DISP-TR treats phase rotation as the primary creative tool.
By stacking all-pass stages in series, spreading their center frequencies, and feeding the network back into itself, it can move from subtle transient contouring to resonant, spectral-smear effects without behaving like a conventional EQ, chorus, or delay.

The core idea is:
- `FREQUENCY` chooses the center of the phase action
- `MOD` scales that center frequency
- `STAGES` and `SERIES` increase complexity and depth
- `SHAPE` spreads the stage distribution around the center
- `JIT` adds controlled jitter movement to frequency, shape, and subtle feedback behavior
- `FEEDBACK` adds resonance and character

## Interface

DISP-TR uses the TR-series text UI with horizontal bar sliders, direct labels, and numeric popup entry.

- **Bar sliders**: Click and drag horizontally. Right-click the value area for numeric entry when available.
- **Main section**: Shows the core disperser controls such as `FREQUENCY`, `MOD`, `FEEDBACK`, `STAGES`, `SERIES`, `SHAPE`, `JIT`, `STYLE`, plus the `ALT` and `MD` toggles.
- **IO section**: Click the triangle toggle bar to switch to the expanded IO view. This exposes `INPUT`, `OUTPUT`, `TILT`, `PAN`, `MIX`, `LIM THRESHOLD`, filter controls, routing/mode combos, invert options, and chaos toggles.
- **Filter bar**: In the IO view, the filter bar opens the HP/LP configuration prompt.
- **MIX MODE**: In the IO view, `INSERT` uses the single `MIX` control. `SEND` exposes separate `DRY LEVEL` and `WET LEVEL` through the split mix control and its numeric prompt.
- **MIDI channel prompt**: Right-click the MIDI channel legend when `MD` is enabled to choose omni or a fixed channel.
- **Gear icon**: Opens the info popup with version, credits, and access to graphics settings.
- **Graphics popup**: Controls CRT tail/post-processing and colour palette options.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

The UI is not paged, but it is stateful: the editor remembers size, palette, CRT setting, MIDI channel, and whether the IO section is expanded.

## Parameters

### STAGES (0-128)

Number of first-order all-pass stages in each chain.
Higher values increase phase complexity and effect intensity.
Smoothed to keep automation and live edits stable.

### SERIES (1-4)

Number of cascaded stage chains.
Each extra series block deepens the effect.
Series changes use a short crossfade to avoid clicks.

### FREQUENCY (20-20000 Hz)

Main center frequency for the dispersion network.
Low values concentrate the effect toward the low end; high values move it upward.
When `MD` is active, MIDI note tracking overrides the slider target.

### MOD (0.25x-4.0x)

Frequency multiplier for the disperser center.
The control is non-linear around `1.0x`, so it gives useful low-ratio and high-ratio ranges without feeling cramped.

### SHAPE (0-100%)

Controls how far the per-stage frequencies spread around the center frequency.

- `0%`: stages cluster tightly around the center
- Higher values: stages fan outward for a broader, more complex phase pattern

### JIT (0-100%)

Adds deterministic internal jitter to the disperser by modulating the effective `FREQUENCY` and `SHAPE` targets, with a very small bounded feedback offset.

- Low values: slow, organic drift
- Mid values: added smooth sample-and-hold movement
- High values: faster extreme layers for more animated instability
- Near `100%`: a very small harmonic/ring-like layer adds extra crunch without directly distorting the audio

The frequency and shape movement uses independent deterministic lanes per active `SERIES`, while feedback jitter stays global because feedback is injected before the full all-pass chain. The modulation is smoothed, bounded, and seeded deterministically so repeated sessions keep a stable character.

### FEEDBACK (-100 to +100%)

Feeds the all-pass output back into the input.

- Positive values emphasize one resonant character
- Negative values invert the feedback polarity and produce a different resonant profile

This is a bipolar, sign-preserving control and is smoothed for live adjustment.

### STYLE (MONO / STEREO / WIDE / DUAL)

Stereo processing mode for the wet disperser path.

- **MONO**: Processes one side and mirrors the result
- **STEREO**: Standard dual-channel processing with matching coefficients
- **WIDE**: Uses complementary coefficient polarity between channels for a broader image
- **DUAL**: Uses an alternate right-channel coefficient set for a more split stereo character

### ALT

Alternates the sign of every other stage coefficient.
This changes the internal phase pattern of the network and gives a different transient and resonance feel without changing the basic control set.

### INPUT (-INF to +24 dB)

Wet-path input gain into the disperser.
This affects the processed signal only, not the dry branch.
The fader floor is -144 dB, displayed as -INF; 0 dB is centered on the control.

### OUTPUT (-INF to +24 dB)

Wet-path output gain after the disperser.
This affects the processed signal only.

### TILT (-6 to +6 dB)

One-knob tilt EQ on the wet path, pivoted around 1 kHz.
Negative values darken the wet signal; positive values brighten it.

### PAN (0-100%)

Global equal-power stereo pan after the wet/dry blend.

- `0%` = left
- `50%` = center
- `100%` = right

### MIX (0-100%)

In `INSERT` mode, this is the standard dry/wet blend:

- `0%` = fully dry
- `100%` = fully wet

### MIX MODE (INSERT / SEND)

Determines how the final blend is controlled.

- **INSERT**: Uses the single `MIX` control
- **SEND**: Uses independent `DRY LEVEL` and `WET LEVEL` gains

### DRY LEVEL / WET LEVEL (SEND mode)

Available when `MIX MODE` is `SEND`.
These set the dry and wet contribution independently instead of using one crossfade-style mix knob.

### HP/LP FILTER

Wet-path high-pass and low-pass filters, configured from the filter popup.

- **HP FREQ / LP FREQ**: 20-20000 Hz
- **HP SLOPE / LP SLOPE**: 6 dB, 12 dB, or 24 dB per octave
- **HP / LP toggles**: enable or disable each filter independently

### FILTER POS

Chooses whether the wet HP/LP filter block and the tilt EQ run before or after the disperser core.

- **F post / T post**
- **F pre / T pre**
- **F pre / T post**
- **F post / T pre**

This is useful because filtering before the all-pass network changes what excites the disperser, while filtering after it shapes the already-dispersed result.

### MODE IN / MODE OUT / SUM BUS

Wet-path routing and mid/side handling controls in the IO view.

- **MODE IN**: `L+R`, `MID`, `SIDE`
- **MODE OUT**: `L+R`, `MID`, `SIDE`
- **SUM BUS**: `ST`, `->M`, `->S`

These are for more advanced routing and matrixing workflows.

### INV POL / INV STR

Post-processing inversion controls.
Each can be applied to:

- **NONE**
- **WET**
- **GLOBAL**

`INV POL` flips polarity.
`INV STR` swaps the stereo channels.

### MD (MIDI)

Enables MIDI note tracking for `FREQUENCY`.
When active, note-on pitch becomes the frequency target.
MIDI velocity influences glide speed, and the channel can be set to omni or a specific input channel via the MIDI channel prompt.

### CHAOS

Dual-target chaos system for movement and instability.

- **CHSF**: Modulates the wet filter cutoffs
- **CHSD**: Adds pre-core micro-delay decorrelation and a subtle wet gain movement

Each target has its own enable toggle.
Both use amount and speed controls in their popup editor:

- **AMOUNT (0-100%)**
- **SPEED (0.01-100 Hz)**

Internally this uses Hermite-interpolated sample-and-hold motion plus drift, so the modulation stays organic instead of stepping harshly.

### LIM THRESHOLD (-36 to 0 dB)

Threshold for the transparent peak limiter.
At `0 dB`, it mainly acts as a safety net.
Lower values make the limiter work harder.

### LIM MODE (NONE / WET / GLOBAL)

Limiter insertion point:

- **NONE**: limiter disabled
- **WET**: limiter on the wet branch after wet input/output gain, before final mixing
- **GLOBAL**: limiter on the final output, after pan

The limiter is a stereo-linked dual-stage design:

- **Stage 1 - leveler**: 2 ms attack, 10 ms release
- **Stage 2 - brickwall**: instant attack, 100 ms release

## Signal Flow

At a high level, DISP-TR processes signal like this:

1. Input arrives
2. `MODE IN` matrix is applied to the wet branch
3. Optional HP/LP filter if `FILTER POS` puts the filter block in `PRE`
4. Optional tilt EQ if `FILTER POS` puts tilt in `PRE`
5. Optional `CHSD` pre-core micro-delay decorrelation and subtle gain movement
6. Disperser core runs (`FREQUENCY`, `MOD`, `STAGES`, `SERIES`, `SHAPE`, `JIT`, `ALT`, `FEEDBACK`, `STYLE`, MIDI note tracking)
7. Optional HP/LP filter if the filter block is in `POST`
8. Optional tilt EQ if tilt is in `POST`
9. `MODE OUT` matrix is applied
10. `INV POL` / `INV STR` in `WET` mode run if selected
11. Wet `INPUT` / `OUTPUT` gain is applied
12. `LIM MODE = WET` limiter runs if selected
13. Dry/wet blend happens (`MIX` or `DRY LEVEL` + `WET LEVEL`)
14. `PAN` is applied
15. `LIM MODE = GLOBAL` limiter runs if selected
16. `INV POL` / `INV STR` in `GLOBAL` mode run if selected
17. Final safety clip remains at the end

This ordering is important: DISP-TR is not just "all-pass, then mix". The IO section meaningfully changes where filtering, tilt, limiting, routing, and inversion happen.

## Technical Details

### DSP Architecture

- **All-pass stage**: first-order topology with per-stage state
- **Coefficient mapping**: frequency is converted to all-pass coefficient space from the current sample rate
- **Stage distribution**: `SHAPE` spreads stage frequencies around the center with a non-linear mapping
- **Jitter**: `JIT` layers deterministic drift, smooth sample-and-hold movement, and an extreme harmonic micro-layer over per-series frequency/shape targets, plus a small bounded feedback offset
- **Feedback**: bipolar and sign-preserving
- **Series topology**: changing `SERIES` crossfades between old and new chain counts
- **Fast path**: when smoothed parameters have settled and no series crossfade is active, the processor uses a tighter inner loop

### Smoothing

Current smoothing behavior includes:

- `FREQUENCY`: EMA smoothing, also used for MIDI note glide
- `STAGES`: smoothed
- `SHAPE`: smoothed
- `JIT`: smoothed before its internal movement layers
- `FEEDBACK`: smoothed
- `INPUT`, `OUTPUT`, `MIX`: smoothed
- `DRY LEVEL` / `WET LEVEL` in `SEND`: smoothed
- `PAN`: smoothed
- `LIM THRESHOLD`: smoothed before limiter application

This keeps rapid GUI movement and automation from producing unnecessary zippering.

### Filters and Chaos

- Wet HP/LP filters use biquad-based filtering with selectable slopes
- `CHSF` modulates filter cutoff movement
- `CHSD` applies pre-core micro-delay decorrelation and subtle gain movement
- Chaos motion uses Hermite interpolation plus drift to stay smooth and less mechanical

### Gain and Safety

- Input/output wet gain faders use the common -144 dB (-INF) to +24 dB curve with 0 dB centered
- Wet gain conversion uses a fast dB-to-linear approximation
- The main limiter is dual-stage and stereo-linked
- A final high-ceiling safety stage remains after the user-facing limiter/invert section

## State Persistence

DISP-TR stores:

- All audio parameters in the main APVTS state
- UI size
- palette and CRT/fx-tail settings
- MIDI channel
- whether the IO section is expanded

## Changelog

### v1.4

Current v1.4 state includes:

- bipolar feedback from `-100%` to `+100%`
- `JIT` movement for deterministic per-series frequency/shape jitter, subtle feedback motion, and high-range harmonic crunch
- dual-target chaos (`CHSF` and `CHSD`)
- wet-path HP/LP filters with slope selection
- IO routing section with `MODE IN`, `MODE OUT`, `SUM BUS`, `MIX MODE`, `FILTER POS`, `INV POL`, and `INV STR`
- dual-stage transparent peak limiter with `WET` and `GLOBAL` placement options
- smoothed live control handling for core continuous parameters and utility gains
