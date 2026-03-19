# 1. Code Structure
[Return to Table of Contents](README.md)

This document describes the directory structure and organisation of the synthesiser implementation.

The source code is structured into modular subsystems to improve maintainability and clearly separate the responsibilities of different components. 

The main implementation resides in the `src/` directory, which is divided into several submodules.

## Project Directory Overview

```
├── doc/
├── include/
├── lib/
├── scripts/
├── src/
│   ├── audio/
│   ├── io/
│   ├── net/
│   ├── ui/
│   ├── config.cpp
│   ├── Knob.cpp
│   └── main.cpp
└── test/
```

## `doc/`

Coursework documentation and report material.

## `include/`

Header files exposed across multiple modules.

These headers define system-wide constants, shared data structures, hardware configuration, and interfaces used by several subsystems.

### `constants.h`

Defines global compile-time constants used throughout the synthesiser system.

This header contains:

- System timing parameters such as the display update interval and keyboard scan interval.
- Musical constants including note frequencies and waveform step sizes.
- Synthesis configuration parameters such as maximum polyphony and command queue length.
- Enumerations used by the input subsystem for identifying rotary encoder controls.

The file also defines lookup tables used for converting musical notes to phase increments used by the synthesis engine. These values are derived from the system sampling rate and phase accumulator modulus. 

### `Knob.h`

Defines the `Knob` class used to represent a rotary encoder with an integrated push button.

The class encapsulates:

- Encoder state tracking.
- Rotation detection logic.
- Button press detection.
- Range-limited parameter control.

Thread-safe access to the knob state is implemented using a FreeRTOS mutex and atomic variables.

This allows the knob state to be safely updated from interrupt contexts while being read by application tasks.

### `pins.h`

Defines the hardware pin mappings used by the synthesiser.

The file provides symbolic constants for:

- Keyboard matrix row and column pins.
- Audio output pins connected to the DAC.
- Joystick analogue inputs.
- Output multiplexer control lines.

Centralising pin definitions in a single header simplifies hardware configuration and improves portability if the hardware platform changes. 

### `profiling.h`

Defines compile-time configuration flags used to enable profiling mode.

When profiling mode is enabled, additional macros control which tasks or interrupt handlers should be instrumented during execution. This mechanism allows selective measurement of execution times without modifying the core application logic.

Profiling mode can also disable normal system components such as threads or interrupt handlers in order to isolate specific code paths for measurement. 

### `sin_lut.h`

Provides a precomputed lookup table used for efficient sine waveform generation.

The table contains 512 samples representing a full sine wave cycle.

During synthesis, the oscillator phase accumulator is used to index this table rather than computing trigonometric functions at runtime.

This lookup-based approach significantly reduces computational overhead during audio generation, which is critical for maintaining deterministic execution in the real-time synthesis task. 

### `SysState.h`

Defines the global system state structure shared across multiple tasks.

The header declares several enumerations used throughout the synthesiser, including waveform types, instrument pre-sets, envelope phases, and synthesiser roles.

It also defines the primary data structures used by the audio and user interface subsystems.

Key structures include:

- `DisplayState`: Stores the parameters currently shown on the OLED display.
- `Sound` : Represents an individual synthesiser voice and its oscillator, envelope, and playback state.
- `SysState` : Maintains global runtime state including input flags, CAN messages, and display parameters.

The global `sysState` instance is protected by a FreeRTOS mutex to ensure safe concurrent access by multiple tasks.

## `lib/`

External or third-party libraries used by the project. These libraries provide hardware abstraction layers and peripheral drivers that simplify interaction with microcontroller peripherals.

### `ES_CAN`

The `ES_CAN` library provides an abstraction layer for the Controller Area Network (CAN) peripheral used by the synthesiser to communicate with other devices.

The library implements the low-level functionality required for CAN communication:

- CAN peripheral configuration.
- Transmission and receipt of CAN frames.
- Interrupt handling for CAN events.
- Buffering of incoming and outgoing messages.

This library is used by the networking subsystem `src/net`to implement higher-level message handling and protocol logic. The `CanProtocol` module encodes and decodes musical control messages and uses the `ES_CAN` interface to transmit and receive frames.

## `scripts/`

Utility scripts used during developments.
These scripts assist with generating source assets and performing development-time tasks that support the synthesiser implementation.

### `sin.py`

Generates the sine lookup table used by the audio synthesis subsystem.

The script computes a discretised sine waveform and writes the result to the header file `include/sin_lut.h`. The generated lookup table contains a fixed number of evenly spaced samples representing one full sine wave period.

The waveform values are scaled to the range 0–255 so they can be stored as 8-bit unsigned integers. During audio synthesis, this table is indexed using the oscillator phase accumulator, allowing sine wave samples to be produced efficiently without performing floating-point trigonometric calculations at runtime. 

Using a precomputed lookup table significantly reduces computational overhead during audio generation and helps maintain deterministic execution time within the real-time audio synthesis task.

## Source Code `src/`

The `src` directory contains the main synthesiser implementation. 
It is divided into subsystem directories reflecting the system architecture.

### Audio Subsystem `src/audio`

#### **`Synth.h`**

Defines the `Synth` class, which manages the core audio generation pipeline including voice allocation, waveform generation, and audio buffer output.

The header declares several supporting types used by the audio subsystem.

`AudioCommandType` enumerates the commands events that can be issued to the synth object, including note activation, note release, sustain hold control, and role changes for  operation with other modules.

`AudioCommand` represents a single command to the synth. 

`GlobalParameters` stores synthesis parameters that apply to all active voices, such as volume, instrument selection, octave, and pitch offset.

The `Synth` class has the following responsibilities:

- Management of a double-buffered audio sample buffer (samplebuffer) used for DMA transfer to the DAC.
- A fixed-size pool of `Sound` voice slots supporting polyphonic playback up to `MAX_VOICES` simultaneous voices.
- Management of an audio command queue through which other subsystems submit note and parameter events to the synth object.
- Voice allocation and deallocation .
- Application of global parameter, updates,such as volume across all active voices.
- Biquad filter state for audio post-processing.

The command queue uses a buffer indexed by separate read and write pointers, allowing input tasks to queue commands without acquiring a mutex.

A  semaphore is used to synchronise buffer production with the DMA transfer interrupt. When the DMA controller signals the filling of the write portion of `samplebuffer` is complete, the interrupt handler releases the semaphore, triggering the samplegen task to start generating the next block of samples.

#### **`Synth.cpp`**

Implements the audio generation pipeline, including initilization of hardware peripherals, such as DMA.

A circular-mode DMA channel is initilialised, which continously transfers elements of the sample buffer directly to the DAC.

The `synth` class implements the following methods:

- `processCommands()` processes all pending audio commands in the queue up to the current write index
- `fillBuffer()` calls processCommands and generates half a sample buffer full of samples by calling `tick()`
- `tick()`  iterates over all voice slots and generates one output sample to be put into `samplebuffer`.
- `UpdateGlobalParams()` writes new global parameters and then `applyGlobalParamUpdates()`  is called to push global change to all active non-held voices

### Input Subsystem `src/io`

The input subsystem is responsible for acquiring and interpreting user interaction from the hardware interface.
This includes scanning the keyboard matrix to detect key presses and processing rotary encoder inputs used to adjust synthesis parameters.

Input data is converted into events that are consumed by the other subsystems, such as the synthesiser engines and UI tasks.

#### **`KeyMatrix.h`**

Declares the `KeyMatrix` class and supporting data structures used for scanning the keyboard matrix.

The header defines:

- The `KeyScanResult` structure which stores the state of all scanned keys and connection flags.
- An interface for performing full keyboard scans and single-row scans.
- The helper functions for controlling the output multiplexer used to select keyboard rows.

The result structure stores both the complete key state and per-column data, which is also reused by the knob input subsystem to detect rotary encoder transitions.

#### **`KeyMatrix.cpp`**

Implements the keyboard scanning logic for the synthesiser.

The implementation performs row-by-row scanning of the keyboard matrix by selecting a row through a multiplexer and reading the corresponding column inputs. Direct GPI register manipulation is used on STM32 platforms to minimise latency and improve scanning performance.

The primary public functions include:

- `scan()` which performs a complete scan of all rows in the matrix and returns the key states.
- `scanRow()` which scans a single row.

The scanning procedure also records the raw column values for each row so that the rotary encoder subsystem can detect state transitions associated with knob rotation.

#### **`KnobManager.h`**

Defines the `KnobManager` class responsible for managing rotary encoder inputs.

The manager maintains an array of `Knob` objects representing the available rotary controls. It provides functions for:

- Subsystem initialisation.
- Updating encoder state from keyboard matrix scans.
- Reading encoder data from an optional I2C expander (for V2 SynthStack hardware only).

When the optional I2C expander configuration is enabled, the class also exposes synchronisation primitives used by the interrupt-driven knob input mechanism.

#### **`KnobManager.cpp`**

Implements the logic for processing rotary encoder inputs and updating synthesiser parameters.

During initialisation, the manager configures the knob hardware and establishes the initial encoder states using data from the keyboard matrix. In the systems that use the I2C expander, the file also contains driver logic for configuring the expander and reading encoder data.

The implementation includes:

- Rotation detection for each encoder.
- Integration with keyboard matrix scanning process.
- Optional interrupt-driven input using I2C port expander.
- FreeRTOS task and ISR wrappers used for asynchronous knob processing.

Encoder updates are translated into parameter adjustments that are later consumed by the synthesis and UI subsystems.

### Networking Subsystem `src/net`

The networking subsystem implements communication between synthesisers using the CAN bus.

It is responsible for encoding musical events into CAN messages, transmitting them over the network, and decoding received messages so they can be applied to the local synthesiser state.

Communication is implemented using an interrupt-driven CAN interface combined with FreeRTOS queues and tasks to decouple hardware interaction from higher-level message processing.

#### **`CanProtocol.h`**

Declares the data structures and interfaces used for CAN communication.

The file defines the message format used to transmit musical events across the network. Each CAN message encodes information about a note event, including the key index, pitch, waveform, octave, and volume parameters. 

It also declares:

- The `NoteMessage` structure used to represent decoded note events.
- Helper functions for encoding and decoding CAN message payloads.
- The `CanProtocol` class, which manages CAN communication state.

The class stores the synchronisation primitives used by the networking subsystem, including:

- A transmit semaphore used to limit concurrent transmissions.
- Queues for received and outgoing messages.

FreeRTOS task and interrupt entry points for CAN reception, transmission, and message decoding are also declared in this header. 

#### **`CanProtocol.cpp`**

Implements the networking logic used to transmit and receive musical events over the CAN bus.

During initialisation, the CAN interface is configured, interrupt handlers are registered, and the FreeRTOS queues and semaphores used for message handling are created.

The implementation includes:

- Encoding and decoding of musical note messages.
- Interrupt handlers for CAN receive and transmit events.
- FreeRTOS tasks responsible for message transmission and decoding.
- Queue-based communication between interrupts and application tasks.

Incoming CAN frames are captured by the receive interrupt and placed into a queue. A decoding task retrieves these messages and converts them into note events that are forwarded to the synthesis engine.

Outgoing messages are generated when local key states change and are placed into a transmit queue. A dedicated transmission task sends these messages to the CAN controller when the transmit semaphore becomes available.

This design separates hardware interrupts from higher-level message processing and ensures that network communication does not interfere with real-time audio generation.

### User Interface Subsystem `src/ui`

#### **`Display.h`**

Declares the `Display` class and the `displayUpdateTask` FreeRTOS task entry point.

The class exposes `begin()` for hardware initialisation and `update()` for one render cycle. Private state holds the U8g2 driver instance, a cached `DisplayState`, and the last CAN message, used to skip `sendBuffer()` calls when content is unchanged.

#### **`Display.cpp`**

Implements the display update logic and, in V2, a custom I2C byte-level callback for the U8g2 library.

`begin()` asserts the display reset line, optionally injects the custom I2C callback, calls `u8g2_.begin()`, and sets the I2C clock to 1 MHz. `update()` reads system state under the mutex, renders a three-line layout (active notes, synthesis parameters, network role and CAN message), and flushes the frame buffer only if `stateChanged()` returns true, avoiding redundant I2C transfers. When `DINO_MODE` is defined, `update()` delegates to `dinoGame.render()` and returns immediately.

`u8x8_byte_rtos_hw_i2c()` is the custom U8g2 callback active when `I2C_EXPANDER_KNOBS` is defined. It acquires and releases `knobManager.i2cMutex` around each I2C transaction to prevent interleaving with `knobTask` reads on the shared bus.

#### **`DinoGame.h`**

Declares the `DinoGame` class and compile-time physics and layout constants (`GROUND_Y`, `DINO_X`, `GRAVITY`, `JUMP_VEL`).

The `DinoState` enumeration defines three states: `DINO_IDLE`, `DINO_PLAYING`, and `DINO_DEAD`. The class exposes `tick(JoyState)` to advance game logic each frame and `render()` to draw to the display.

#### **`DinoGame.cpp`**

Implements the game logic, physics, and sprite rendering for the optional display-based mini game.

Sprite data is stored as static XBM byte arrays: two alternating dinosaur run frames, a dead frame, and a cactus obstacle sprite. All sprites are drawn using `u8g2.drawXBMP()`.

`tick()` drives the state machine, transitioning between idle, playing, and dead states on rising-edge joystick input. While playing it calls `updatePhysics()`, `updateObstacle()`, and `checkCollision()` each frame.

`updatePhysics()` applies gravity and ground clamping each frame, with a jump initiated by setting `velY_` to `JUMP_VEL` on a joystick press. `updateObstacle()` scrolls the cactus left, wrapping it to the right edge on exit, incrementing the score, and increasing speed every ten obstacles. Collision detection uses AABB with a 2-pixel forgiveness margin.

`render()` composes the full frame — ground, dinosaur, obstacle, and score — and calls `sendBuffer()` unconditionally each frame. The dinosaur sprite alternates between run frames every four frames when on the ground.

## System Configuration `config.cpp`

Provides the system clock configuration used during microcontroller startup.

This file overrides the default clock initialisation provided by the STM32 HAL for the NUCLEO-L432KC board in order to support the hardware requirements of the StackSynth module. The configuration is executed during system start-up before the application code begins running. 

The implementation includes:

- Configuration of the MSI oscillator as the primary clock source.
- Initialisation of the phase-locked loop (PLL) to generate the system clock.
- Configuration of AHB and APB bus clock divisors.
- Setup of peripheral clocks used by the system.

## Hardware Control `Knob.cpp`

Implements the logic for representing and updating the state of a rotary encoder control.

The file provides the implementation of the `Knob` class, which tracks the rotation value and push-button state of an individual encoder. Each knob maintains its current value within a defined range and updates this value based on detected encoder transitions.

The implementation includes:

- Detection of quadrature encoder transitions.
- Direction tracking for handling ambiguous state changes.
- Clamping of encoder values to defined limits.
- Atomic updates to ensure safe access from concurrent contexts.
- Detection of push-button press events associated with the encoder.

Rotation changes are detected by comparing the current encoder state with the previous state and determining the direction of movement. Updates to the encoder value are performed using atomic operations so that the knob state can be safely accessed by multiple tasks and interrupt handlers.

## System Entry Point `main.cpp`

Implements the system entry point and initialisation logic for the synthesiser application.

The file is responsible for configuring hardware peripherals, initialising software subsystems, and creating the FreeRTOS tasks that implement the concurrent behaviour of the system. Global instances of shared components such as the system state, input devices, synthesiser engine, and networking interface are also defined here.

The implementation includes:

- Hardware initialisation, including GPIO pin configuration.
- Initialisation of shared system state and synchronisation primitives.
- Setup of subsystem components such as the display, synthesiser engine, and CAN interface.
- Creation and scheduling of FreeRTOS tasks responsible for audio generation, input scanning, networking, and display updates.
- Optional profiling utilities used to measure task and interrupt execution times.

During normal operation, the `setup()` function performs system initialisation and starts the FreeRTOS scheduler. The system then operates entirely through concurrent tasks, while the Arduino `loop()` function is repurposed for profiling when profiling mode is enabled.