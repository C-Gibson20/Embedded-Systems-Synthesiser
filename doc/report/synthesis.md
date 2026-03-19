# 2. Audio Synthesis System
[Back to Table of Contents](README.md)

The audio synthesis subsystem is responsible for generating digital waveform samples that are streamed to the DAC at a fixed sampling rate. To achieve deterministic and efficient waveform generation, the system employs a phase accumulator-based method.

Each active voice in the synthesiser is represented by a state structure containing the parameters required for waveform generation. These parameters include:

- A phase accumulator representing the current waveform phase.
- A phase step size determining the output frequency.
- A waveform type.
- A volume level.

During each audio update cycle, the synthesis engine iterates over all active voices and generates the corresponding waveform samples. The processing sequence is as follows:

1. The phase accumulator for each voice is incremented by the configured step size.
2. The updated phase value is used to evaluate the selected waveform function.
3. The resulting sample values from all active voices are summed to produce a mixed output signal.
4. The final signal is scaled to prevent numerical overflow and audio clipping.

The synthesiser supports several waveform types, including square, sawtooth, triangle, sine, supersaw and sine-fold variants. Multiple voices can be active simultaneously, allowing the system to produce polyphonic output and support chord playback.