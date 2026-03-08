# DISP-TR v1.0c

<br/><br/>

<img width="449" height="450" alt="image" src="https://github.com/user-attachments/assets/639141ef-16a7-46fd-bc56-98751541f616" />

<br/><br/>

DISP-TR is a phase-dispersion audio effect built on cascaded all-pass filters.  
It reshapes transients and phase response without classic EQ-style magnitude boosts/cuts, creating tighter, softer, or more smeared attack behavior depending on settings.

## Concept

DISP-TR treats phase rotation as the primary creative tool. By stacking all-pass stages in series chains with adjustable frequency distribution, it produces effects ranging from subtle transient reshaping to extreme spectral smearing — all without altering the magnitude spectrum.

The reverse mode builds a time-reversed impulse response from the same all-pass network and applies it via partitioned convolution, producing non-causal phase behavior that feels like the signal is "un-dispersing" itself.

## Interface

DISP-TR uses a text-based UI with horizontal bar sliders. All controls are visible at once — no pages, tabs, or hidden menus.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: RVS (reverse), INV (invert). Click to enable/disable.
- **Gear icon** (top-right): Opens the info popup with version, credits, and a link to Graphics settings.
- **Graphics popup**: Toggle CRT post-processing effect and switch between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

The value column to the right of each slider shows the current state in context:
- STAGES shows the number of active all-pass stages.
- SERIES shows the number of cascaded chains.
- FREQUENCY shows Hz.
- SHAPE shows the distribution parameter.

## Parameters

### STAGES (0–128)

Number of all-pass stages used in each series chain.  
Higher values increase phase complexity and effect intensity.  
Smoothed linearly (2 s time constant) for artifact-free transitions during automation.

### SERIES (1–4)

Number of cascaded chains.  
Each chain is a full copy of the stage network. Higher values deepen the overall dispersion behavior.

### FREQUENCY (20–20000 Hz)

Main frequency focus for phase redistribution.  
Low values push the dispersion character toward the low end; high values move it upward.  
Smoothed linearly (1 s time constant).

### SHAPE (−1 to +1)

Controls how spread or warped the per-stage frequency distribution is around FREQUENCY.  
At 0 all stages share the same coefficient. Moving away from centre fans the stages out across the spectrum.  
Smoothed linearly (100 ms time constant).

### RVS (Reverse)

Enables reverse-style processing. Instead of applying the all-pass chain directly, DISP-TR builds a forward impulse response through the chain, reverses it, and loads it into a partitioned convolution engine (1024-sample latency).

A rebuild is triggered when STAGES, SERIES, FREQUENCY, or SHAPE change, with a settle window (120 ms stability + 200 ms minimum interval) to avoid excessive recalculations during fast automation.

### INV (Invert)

Inverts output polarity (multiplies signal by −1).

## Technical Details

### DSP Architecture
- **All-pass filter**: First-order, `y = coeff * (x − z1) + z1` with per-stage state.
- **Coefficient**: `tan(π * frequency / sampleRate)` mapped through `(1 − c) / (1 + c)`.
- **Stage distribution**: SHAPE fans stage frequencies around FREQUENCY using a power-curve mapping.
- **Smoothing**: Linear `SmoothedValue` per parameter (STAGES 2 s, FREQUENCY 1 s, SHAPE 100 ms).
- **Reverse mode**: Forward IR → time-reverse → `juce::dsp::Convolution` with 1024-sample partitioned latency.
- **Rebuild guard**: 120 ms settle window + 200 ms cooldown between convolution rebuilds.

### State Persistence
- All parameters saved via JUCE AudioProcessorValueTreeState.
- UI state (window size, palette, CRT toggle, custom colours) persisted separately in the processor's state block.
