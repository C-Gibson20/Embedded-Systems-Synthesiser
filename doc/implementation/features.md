# 4. Features
[Return to Table of Contents](README.md)

This document describes the functional features implemented in the synthesiser system.  These features are built on top of the architecture described in the report and are implemented using the modules documented in the [modules.md](modules.md) file.

The following video clearly demonstrates the advanced features implemented in our system.

TODO Video showing off feature

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

TODO Jeremy

Possible topics:

- system state display
- parameter feedback
- user interface elements
- The OLED display is interfaced using the U8g2 graphics library, which requires a platform-specific byte-level communication callback. In the V2 system, a custom implementation, `u8x8_byte_rtos_hw_i2c`, is provided to integrate the display driver with the underlying STM32 hardware and FreeRTOS environment.
    
    This function replaces the default blocking I2C implementation supplied by the library, and instead utilises the STM32 hardware I2C peripheral. By leveraging hardware-driven transfers, the implementation reduces CPU overhead and improves transfer efficiency.
    
    The custom interface is designed to be compatible with the RTOS environment. In particular, it allows the scheduler to interleave other tasks during longer transfers.
    
    As a result, the display update task benefits from improved execution efficiency and more predictable timing behaviour. This is important given that display updates involve transferring relatively large frame buffers over I2C.
    

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

TODO Jeremy

Possible topics:

- ASDR
- Biquad
- --

### Display-Based Mini Game

TODO Jeremy

Possible topics:

- rendering logic
- input interaction
- compile-time feature enabling
- --