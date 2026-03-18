# Project Overview

## Hardware

**MCU:** STM32L432KC (Cortex-M4, 80 MHz, 256 KB flash, 64 KB RAM)

**Audio output:** Built-in 8-bit DAC at 22 kHz, driven by Timer 6 via DMA (circular mode, double buffer)

---

## Features

### Audio Synthesis
- Polyphonic — up to 16 simultaneous voices
- DMA double-buffering: CPU fills 64-sample batches while DMA streams the other half to the DAC
- Per-voice ADSR envelope (attack, decay, sustain, release) — rates set per instrument preset
- Mix-level 2nd-order Butterworth biquad low-pass filter — coefficients precomputed per instrument in Q1.14 fixed-point
- Per-instrument loudness gain normalisation to compensate for RMS differences between waveforms

### Instruments
Four instrument presets selectable via the waveform knob (positions 0–3):

| # | Instrument | Oscillator | Filter cutoff |
|---|---|---|---|
| 0 | Piano | Saw | 3500 Hz |
| 1 | Midi | Square | None |
| 2 | Violin | SuperSaw | 2000 Hz |
| 3 | Flute | Sine | 4500 Hz |

See [instruments.md](instruments.md) for full preset details, ADSR timings, biquad coefficient derivation, and design rationale.

### Networking
- CAN bus multi-module support — SENDER, RECEIVER, SINGLE roles
- Auto-detection handshaking — see [handshaking.md](handshaking.md)
- Notes and instrument selection synchronised across modules

### Dino Game
Enabled with `#define DINO_MODE` in `platformio.ini`. Replaces the synth display with a side-scrolling obstacle game controlled via the joystick. The synthesiser continues to run in the background.

---

## Build Flags

Defined in `platformio.ini` or `profiling.h`:

| Flag | Effect |
|---|---|
| `DINO_MODE` | Enables the dino game display task |
| `V1` / `V2` | Selects hardware revision for knob matrix wiring |
| `I2C_EXPANDER_KNOBS` | Uses I2C expander for knob input (V2) |
| `PROFILING_MODE` | Enables WCET profiling — see below |

---

## Profiling

Enable by uncommenting `#define PROFILING_MODE` in `include/profiling.h`. This also defines `DISABLE_THREADS` and `DISABLE_ISRS`, removing FreeRTOS scheduling so tasks can be timed directly.

Each task is run for 32 iterations and the average execution time printed over Serial.

### Tasks profiled

| Define | Task | WCET setup |
|---|---|---|
| `PROFILE_SAMPLEGEN` | `sampleGenTask` | All 16 voices active, SINEFOLD waveform (worst-case) |
| `PROFILE_DINO` | `dinoTask` (tick + render) | Game in playing state, JOY_UP input |
| `PROFILE_SCANKEYS` | `scanKeysTask` | — |
| `PROFILE_DISPLAY` | `displayUpdateTask` | — |
| `PROFILE_DECODE` | `decodeTask` | Command queue reset before each run |
| `PROFILE_CAN_TX` | `CAN_TX_Task` | 2ms delay between iterations |

### ISRs profiled

| Define | ISR |
|---|---|
| `PROFILE_CAN_RX_ISR` | `CAN_RX_ISR` |
| `PROFILE_CAN_TX_ISR` | `CAN_TX_ISR` |
| `PROFILE_KNOB_ISR` | `knobISR` (I2C expander only) |

`PROFILE_DINO` is only available when `DINO_MODE` is also defined. `PROFILE_KNOB` / `PROFILE_KNOB_ISR` are only available when `I2C_EXPANDER_KNOBS` is defined.

---

## Documentation

| Document | Contents |
|---|---|
| [instruments.md](instruments.md) | Instrument presets, ADSR, biquad filter, gain normalisation |
| [doubleBuffer.md](doubleBuffer.md) | DMA double-buffering design |
| [handshaking.md](handshaking.md) | CAN auto-detection protocol |
| [filters-and-effects.md](filters-and-effects.md) | Feasibility analysis of additional audio effects |
| [StackSynth-v1.pdf](StackSynth-v1.pdf) | V1.1 hardware schematic |
| [StackSynth-v2.pdf](StackSynth-v2.pdf) | V2.1 hardware schematic |
