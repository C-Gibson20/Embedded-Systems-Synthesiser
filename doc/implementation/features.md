# 4. Features
[Return to Table of Contents](README.md)

This document describes the functional features implemented in the synthesiser system.  These features are built on top of the architecture described in the report and are implemented using the modules documented in the [modules.md](modules.md) file.

## Demonstrations

**CAN-Based Distributed Synthesis**

https://github.com/user-attachments/assets/94364843-ae49-44b5-b197-1cc3da56e4ed
<!-- <video src="../CAN.mp4" controls width="600"></video> -->
[CAN-Based Distributed Synthesis Video](../CAN.mp4)

**Instrument Presets and Advanced Waveforms**

https://github.com/user-attachments/assets/65f04ced-008e-4458-b12b-2c6a1a11a4ac
<!-- <video src="../Instruments.mp4" controls width="600"></video> -->
[Instrument Presets and Advanced Waveforms Video](../Instruments.mp4)

**Held Notes and Polyphony**

https://github.com/user-attachments/assets/5124a1bb-8d5a-4f10-9eff-a028388088a4
<!-- <video src="../Held.mp4" controls width="600"></video> -->
[Held Notes and Polyphony Video](../Held.mp4)

**Display-Based Mini Game**

https://github.com/user-attachments/assets/3525706e-4e00-4314-abe7-5119c1b3943a
<!-- <video src="../Dino.mp4" controls width="600"></video> -->
[Display-Based Mini Game Video](../Dino.mp4)

## Core Features

### Real-Time Audio Synthesis

The system implements real-time audio synthesis using a phase accumulator-based oscillator model. Each active voice maintains its own phase, waveform type, pitch, and amplitude parameters.

Polyphonic voice handling is achieved by maintaining an array of active voices. During each audio update cycle:

- Phase accumulators are incremented.
- Waveform samples are generated for each active voice.
- All voice outputs are summed and scaled.

Audio output is produced via a hardware-assisted pipeline:

- Samples are written to a buffer in memory.
- DMA transfers the buffer to the DAC.
- Timer 6 triggers sample output at a fixed rate.

This architecture ensures continuous audio output with minimal CPU overhead and deterministic timing.

### Keyboard Input

Keyboard input is acquired through a matrix-scanned interface. The key matrix is scanned periodically, and key state transitions are used to detect note-on and note-off events.

Detected key events are translated into synthesis commands:

- Note-on events activate a voice in the synthesiser.
- Note-off events release the corresponding voice.

The system also supports held notes, allowing notes to remain active without continuous key press. A note can be held by pressing the associated rotary encoder, enabling chords to be constructed incrementally.

Held notes retain the synthesis parameters (e.g. waveform, pitch, octave) present at the time they were triggered. Subsequent parameter changes do not affect these notes, ensuring consistent playback behaviour.

### Rotary Encoder Control

Rotary encoders provide real-time control of synthesis parameters.

Each encoder is mapped to a specific parameter:

- Pitch.
- Waveform selection.
- Volume.
- Octave.

Encoder rotation updates the corresponding parameter value, which is applied to newly generated notes.

The octave control supports two modes:

- Local mode: Adjusts the octave of locally generated notes.
- Offset mode: Applies an octave offset to received network notes.

The control mode is toggled by pressing the octave encoder knob. This dual-function design allows efficient control of both local and distributed synthesis behaviour using a single input.

Encoder inputs are processed using quadrature decoding and atomic state updates, ensuring responsive and thread-safe operation.

### OLED Display Interface

The OLED display provides real-time visual feedback of the synthesiser state, updated at 10 Hz by a dedicated FreeRTOS task. Three lines are rendered:

- **Line 1 - Active notes:** Names of all currently sounding notes.
- **Line 2 - Synthesis parameters:** Pitch offset, waveform, octave (local or offset mode), and volume.
- **Line 3 - Network role and CAN message:** Current device role (Sender / Receiver / Single) and the most recent CAN message payload.

To reduce I2C bus load, `sendBuffer()` is only called when the displayed content has changed since the last frame.

In V2, a custom U8g2 I2C callback (`u8x8_byte_rtos_hw_i2c`) replaces the default blocking implementation. It uses the STM32 hardware I2C peripheral and acquires the shared `i2cMutex` around each transaction, preventing conflicts with the knob expander on the same bus. This significantly reduces the WCET of `displayUpdateTask` compared to V1. In practice, real-world execution time is lower still, as typical frames are sparse and `sendBuffer()` is frequently skipped - see [Profiling](profiling.md) for further discussion.


## Networking Features

### CAN-Based Distributed Synthesis

The system supports distributed synthesis using CAN bus communication. Musical events generated on one device can be transmitted and reproduced on other connected synthesisers.

Senders do not play notes locally, they  just transmit them to receivers who then play them. Receivers play received notes and locally played notes.

Key events are encoded into fixed-size CAN messages containing:

Incoming messages from senders are processed by a dedicated decoding task, which converts them into synthesiser commands at the receiver.

The system supports both default and custom device roles:

- By default, the left-most keyboard acts as the sender, and others act as receivers.
- In custom configuration mode, devices can be dynamically assigned as senders or receivers

This mode is toggled using a rotary encoder button, allowing flexible network configurations without recompilation.

Queue-based message handling and interrupt-driven reception ensure that network activity does not interfere with real-time audio processing.

## Additional Features

### Polyphonic Playback

The synthesiser supports simultaneous playback of multiple notes through a polyphonic voice model.

Each voice operates independently, allowing:

- Chord generation.
- Overlapping note playback.
- Independent parameter assignment per voice.

Voice allocation is managed dynamically. When a note is triggered, an available voice is assigned and configured with the current synthesis parameters.

Held notes and incoming network notes are integrated into the same voice system, enabling seamless combination of local and distributed inputs.

This design allows expressive performance while maintaining deterministic execution within the real-time audio task.

### Advanced Waveforms

In order to allow the synthesiser to output a larger variety of sounds, support was for additional waveforms was added to our synthesizer, each of which has different properties. If these properties are combined with  ADSR (which is described below) different instruments can be emulated. All the waveforms that can be generated, along with the instrument they emulate, and their generation methods are as follows:

| Waveform | Instrument | Generation Method |
| --- | --- | --- |
| Square | MIDI | Threshold comparison of the phase index; outputs maximum or minimum value depending on phase. |
| Sawtooth | Piano | Direct output of the phase index, which is a linearly rising ramp that resets each cycle. |
| Sine | Violin | Indexed lookup into a 512-entry precomputed table. |
| Supersaw | Flute | Averages of two sawtooth waves with a fixed phase offset. |

Waveform generation is performed using a combination of lookup tables (e.g. sine LUT) and analytical functions for other waveforms. This avoids expensive runtime computation and ensures deterministic execution time.

### Advanced Sound Effects

Each voice passes through an ADSR envelope and a per-instrument biquad low-pass filter, giving each waveform a distinct tonal character that evolves over the duration of a note.

**ADSR Envelope**

Each active voice runs an independent ADSR state machine. The envelope level (0–65535) is updated every sample at 22 kHz and scales the voice output amplitude. Attack, decay, sustain, and release rates are defined per instrument preset. On note-off, the voice transitions to the release phase and is freed once the envelope reaches zero, allowing notes to decay naturally rather than cutting off abruptly.

**Biquad Low-Pass Filter**

A 2nd-order Butterworth low-pass filter is applied to the mixed output each sample. Coefficients are precomputed offline per instrument in Q1.14 fixed-point arithmetic, avoiding any floating-point computation at runtime. The cutoff frequency is tuned per instrument. The filter was developed from an initial single-pole IIR low-pass design and extended to a biquad for a steeper roll-off. Since coefficients are computed offline, more sophisticated filter designs, such as higher-order or band-pass variants, could be substituted with no additional runtime cost.

**Instrument Presets**

Waveform selection simultaneously applies a full preset combining ADSR rates, biquad coefficients, and a gain normalisation factor. This allows a single encoder control to switch between meaningfully distinct sounds rather than just changing the raw waveform shape.

### Display-Based Mini Game

An optional side-scrolling dinosaur game rendered on the OLED display. Enabled at compile time by uncommenting `-D DINO_MODE` in `platformio.ini`, with zero runtime overhead when disabled.

The player jumps over approaching cacti using the joystick, with speed increasing every ten obstacles. See [Display](../implementation/code_structure.md#display) and [DinoGame](../implementation/code_structure.md#dinogame) for implementation details.
