# DISP-TR v1.0

<img width="449" height="450" alt="image" src="https://github.com/user-attachments/assets/639141ef-16a7-46fd-bc56-98751541f616" />

<br/><br/>


DISP-TR is a phase-dispersion audio effect based on cascaded all-pass filters.  
It reshapes transients and phase response without classic EQ-style magnitude boosts/cuts, creating tighter, softer, or more smeared attack behavior depending on settings.

## What it does

- Processes the signal through multiple all-pass stages in one or more series chains.
- Changes the distribution of phase rotation across the spectrum with `Frequency` and `Shape`.
- Provides a reverse-mode (`RVS`) flavor that uses a time-reversed impulse-style response.
- Offers polarity inversion (`INV`) after processing.

## Parameters

- **Stages**
	- Number of all-pass stages used in each chain.
	- Higher values increase phase complexity and effect intensity.

- **Series**
	- Number of cascaded chains.
	- Higher values deepen the overall dispersion behavior.

- **Frequency**
	- Main frequency focus for phase redistribution.
	- Low values push character lower in the spectrum; high values move it upward.

- **Shape**
	- Controls how spread/warped the stage frequency distribution is around `Frequency`.
	- Lower/higher values change how concentrated vs. wide the dispersion feels.

- **RVS**
	- Enables reverse-style processing mode.
	- Uses an internally generated reversed impulse response approach for non-causal style behavior.

- **INV**
	- Inverts output polarity (multiplies signal by `-1`).

## Notes

- Parameter smoothing is implemented for `Stages`, `Frequency`, and `Shape` to improve transition behavior during control changes.
- UI state (size and palette options) is persisted.
- Some internal/debug parameters may exist in project state but are not part of the normal end-user workflow (UI Width & Height)
