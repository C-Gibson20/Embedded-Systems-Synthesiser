# 2. Module Documentation
[Return to Table of Contents](README.md)

This document describes the detailed implementation of the synthesiser system.  

## Source Code `src/`

The `src` directory contains the main synthesiser implementation. 
It is divided into subsystem directories reflecting the system architecture.

### Audio Subsystem `src/audio`

#### **`Synth.h` and `Synth.cpp`**

The `Synth` module implements the audio generation pipeline. It generates polyphonic audio output using DMA-driven DAC conversion, supports multiple instrument presets with ADSR envelopes and biquad filtering, and uses a lock-free command queue to receive note events from other subsystems.

<br>

**Hardware Initialisation**

The module configures three STM32 peripherals to produce continuous audio output:

- **TIM6** is configured as a timer with a period of 3636 ticks at 80 MHz, producing an update event at approximately 22 kHz. This event serves as the DAC trigger.
- **DAC1 Channel 1** is configured to convert one sample on each TIM6 trigger. The output buffer is enabled to drive the analogue output pin directly.
- **DMA1 Channel 3** transfers samples from the sample buffer to the DAC data register in circular mode. The buffer is 128 bytes, divided into two halves. When the DMA completes the first half, it raises a half-transfer interrupt. When it completes the second half, it raises a transfer-complete interrupt.

This arrangement allows the CPU to fill the write portion of the sample buffer while the DMA streams the other half to the DAC, ensuring continuous audio output.

<br>

**DMA Callbacks**

`HAL_DAC_ConvHalfCpltCallbackCh1()` and `HAL_DAC_ConvCpltCallbackCh1()` are called from the DMA transfer interrupt. Each callback sets the `writeBuffer1` flag to indicate which buffer half is should be written to, then gives the `sampleBufferSemaphore` to trigger the samplegen task to start generating the next block of samples.

Only minimal work is performed in interrupt context. The semaphore handoff ensures that all synthesis computation occurs in a task context.

<br>

**Instrument Presets**

The `InstrumentPreset` structure defines the timbral character of each instrument:

```cpp
struct InstrumentPreset {
    SynthWaveform waveform;
    uint16_t attackRate, decayRate, sustainLevel, releaseRate;
    uint8_t gain;
    int16_t b0, b1, b2, a1, a2;
};
```

Each preset specifies:

- The oscillator waveform shape.
- ADSR envelope rates, computed as increments per sample on a 0–65535 scale at 22 kHz.
- A gain value for loudness normalisation across instruments, applied as `(sample * gain) >> 7` where 128 represents unity.
- Biquad low-pass filter coefficients in Q1.14 fixed-point format, computed offline as a second-order Butterworth filter at the target cutoff frequency.

Four presets are defined: Piano (sawtooth, 3500 Hz cutoff), MIDI (square, no filter), Violin (supersaw, 2000 Hz cutoff), and Flute (sine, 4500 Hz cutoff).

<br>

**Waveform Functions**

Six waveform generators are implemented as static inline functions. Each takes an 8-bit phase index and returns an unsigned 8-bit amplitude value.

- `funcSquare` returns 255 for the first half of the cycle and 0 for the second.
- `funcSaw` returns the phase index directly.
- `funcTri` ramps up for the first half and ramps down for the second.
- `funcSine` looks up the value from a precomputed 512-entry sine table.
- `funcSuperSaw` mixes two detuned sawtooth waves for a thicker sound.
- `funcSineFold` applies wavefold distortion to the sine output.

<br>

**Lock-Free Command Queue**

Communication between other subsystems and the synth object uses a ring buffer indexed by separate read and write pointers.

`pushAudioCommand()` is called by other subsystems to submit audio commands, note release, sustained held notes, and role change events to the synth object without acquiring a mutex. The write is performed inside a critical section to ensure atomicity of the update. A data memory barrier ensures the command data is visible before the write pointer advances.

`processCommands()` is called at the start of each buffer fill. It reads the write index once, issues a memory barrier, and then processes all pending audio commands up to the current write index. This avoids locking between the producer and consumer.

The queue has a fixed capacity of 32 entries, to absorb bursts from key scanning and CAN reception without overflow.

<br>

**Sound Allocation**

The synth object maintains a fixed-size pool of `Sound` voice slots supporting polyphonic playback up to `MAX_VOICES` simultaneous voices. Voice allocation and deallocation is managed using a free-list stack.

`allocateSound()` pops the top index from `freeSounds_[]` and returns it. If no voices are available it returns -1 and the note is silently dropped.

`freeSound()` pushes the index back onto the stack when a voice finishes its release phase.

This avoids linear searches for free slots and provides constant-time allocation.

<br>

**`fillBuffer()`**

Performs one complete buffer-fill:

1. Calls `processCommands()` to apply any pending audio commands.
2. Determines the write portion of the sample buffer from the `writeBuffer1` flag.
3. Generates half a sample buffer full of samples by calling `tick()` in a loop.
4. Computes the `activeNotesBitmask` for display use.

The bitmask is a single 16-bit word write, which is atomic on ARM Cortex-M4. This allows the display task to read it without synchronisation.

<br>

**`tick()`**

Iterates over all voice slots and generates one output sample to be put into the sample buffer. The function performs four stages.

*Voice mixing.* For each active voice slot it advances the ADSR envelope state machine, accumulates the phase, generates the waveform sample, applies volume scaling, gain normalisation, and envelope modulation, and sums the result into a signed mixer accumulator.

*Automatic gain control.* If more than one voice is active, the mixed output is divided by the number of active voices using a precomputed reciprocal table to avoid runtime division.

*Biquad filtering.* A second-order Butterworth low-pass filter is applied as audio post-processing using the current instrument's coefficients in Q1.14 fixed-point arithmetic.

*Output clamping.* The filtered output is centred at 128 and clamped to the 0–255 range to prevent overflow.

<br>

**Global Parameter Updates**

`updateGlobalParams()` is called by `scanKeysTask` when a knob value changes. It writes the new global parameters into the `GlobalParameters` structure, which uses `volatile` fields to ensure visibility across tasks, followed by a memory barrier.

`applyGlobalParamUpdates()` is called immediately after to push the global change to all active non-held voices. It applies the current instrument preset, volume, pitch, and recomputed step sizes. Held and remote voices are excluded to preserve their original parameters.

<br>

##### **Runtime Optimisations**

Audio output uses DMA circular mode with double buffering. The CPU fills 64 samples into the sample buffer while the DMA streams the other half to the DAC. This eliminates a sample interrupt thread, which has overhead.

The ADSR envelope, gain normalisation, and volume scaling are applied in a single expression rather than as separate multiply-and-shift stages. This reduces the number of operations per voice per sample.

### Input Subsystem `src/io`

The input subsystem is responsible for acquiring user interaction from the hardware interface and converting it into events that influence synthesiser behaviour. This includes keyboard matrix scanning for note input and rotary encoder processing for parameter control.

#### **`KeyMatrix.h` and `KeyMatrix.cpp`**

The `KeyMatrix` module implements scanning of the keyboard matrix used to detect key presses and device connectivity. The matrix consists of 7 rows $\times$ 4 columns, producing 28 key inputs plus connection detection lines.

<br>

`KeyScanResult` stores the result of a full keyboard can.

```cpp
struct KeyScanResult {
	std::bitset<32> inputs;
	bool westConnected;
	bool eastConnected;
	std::array<std::bitset<4>, 7> rowData;
};
```

This structure contains:

- A bitset representing the state of all keys.
- Connection flags for neighbouring synthesisers.
- Raw row data used by the knob subsystem to detect encoder transitions.

This allows the keyboard scan to detect key presses, and update encoder state.

<br>

`KeyScanResult scan()`

Performs a complete scan of the keyboard matrix:

1. Iterates through all matrix rows.
2. Selects the active row using a multiplexer.
3. Reads the column inputs.
4. Stores the results in the `KeyScanResult` structure.

The scan result is then used by the `scanKeysTask` to detect key state changes.

<br>

#### **Runtime Optimisations**

Row selection and column reads use direct register manipulation rather than Arduino digitalWrite and digitalRead. This avoids the overhead of higher-level abstractions and significantly reduces scan latency.

Only a short microsecond delay `delayMicroseconds(3)` after selecting a row. This ensures physical pin stability while keeping scanning cycle fast.

The use of `std::bitset` allows compact storage and efficient bit-level operations when detecting key changes.

<br>

#### **`KnobManager.h` and `KnobManager.cpp`**

The `KnobManager` module manages the rotary encoder inputs used to control synthesiser parameters such as pitch, waveform, octave, and volume.

<br>

`KnobManager::begin(KeyMatrix &matrix)`

Initialises the knob subsystem:

1. Configures the output multiplexer mode.
2. Initialises each `Knob` instance.
3. Reads the initial encoder states from the keyboard matrix.
4. Optionally initialises the I2C encoder expander.

Initialising encoder states prevents false rotations from being detected when the system starts.

<br>

`updateRotations(uint8_t rowIdx, std::bitset<4> cols, OctaveControlMode mode)`

Updates encoder states using the latest matrix scan results. The function interprets specific row/column combinations as quadrature signals for rotary encoders and forwards these transitions to the associated `Knob` objects.

<br>

`readAll(OctaveControlMode mode)` 

Used when the system is configured with an I2C port expander for knob inputs. The function:

1. Reads encoder states from the I2C device.
2. Decodes quadrature signals.
3. Updates the associated `Knob` objects.

<br>

**Interrupt and Task Integration**

The knob subsystem integrates with FreeRTOS through two components.

`knobISR()`: Interrupt service routine triggered by the I2C expander. The ISR signals the knob processing task using a semaphore.<br>
`knobTask()`: FreeRTOS task responsible for processing encoder updates when interrupts occur.

The task waits for a semaphore signal before reading encoder data and updating knob state.

<br>

#### **Runtime Optimisations**

Encoder transitions are extracted directly from keyboard scan results. This avoids performing separate GPIO reads for the encoder subsystem.

When the I2C expander configuration is enabled, encoder processing becomes interrupt driven, reducing unnecessary polling.

The underlying `Knob` class uses atomic operations to update rotation values safely when accessed by multiple tasks. This ensure thread-safe behaviour without introducing expensive locking operations.

<br>

### Networking Subsystem `src/net`

The networking subsystem implements communication between synthesisers using the CAN bus. It allows musical events generated on one device to be transmitted and reproduced on other synthesisers connected to the network.

#### **`CanProtocol.h` and `CanProtocol.cpp`**

`NoteMessage`

Represents a decoded musical event received from the network.

```cpp
struct NoteMessage {
	uint8_t type;
	uint8_t keyIdx;
	int8_t pitch;
	uint8_t waveform;
	uint8_t octave;
	uint8_t volume;
};
```

The structure allows CAN payloads to be converted into synthesiser commands that can be passed to the audio subsystem.

<br>

`decodeMsg()` 

Converts the raw 8-byte CAN payload into a `NoteMessage` structure. This avoids the repeated indexing into the byte array during later processing.

<br>

`encodeNoteOn()`: Creates a CAN message representing a note-on event. Parameters encoded:

- Key index.
- Pitch.
- Waveform.
- Octave.
- Volume.

`encodeNoteOff()`: Creates a CAN message representing a note-off event. 

These helper functions allow message encoding to occur with minimal overhead and avoid repeated manual array manipulation.

<br>

The `CanProtocol` class manages the runtime state of the networking subsystem.

Key resources maintained by the class include:

- `msgInQ` which is the receive queue that stores incoming CAN messages.
- `msgOutQ` which is the transmit queue that buffers outgoing messages.
- `txSemaphore` which is the transmit semaphore that limits concurrent transmissions.

These resources allow the networking subsystem to interact safely with FreeRTOS tasks and interrupts.

`CanProtocol::begin()`

Initialises the CAN subsystem:

- Initialises the CAN peripheral.
- Configures the message filter.
- Registers interrupt handlers.
- Creates FreeRTOS queues and semaphores.

Queues are created with a capacity of 36 messages to accommodate bursts of network activity without blocking tasks.

<br>

`handleKeyChange()`

Detects key state changes and generates the corresponding network message.

Inputs include:

- Current key state.
- Previous key state.
- Synthesiser parameters.

The method:

1. Determines whether the key was pressed or released.
2. Encodes the corresponding CAN message.
3. Places the message into the transmit queue.

An early-exit optimisation is used that prevents unnecessary message generation when the key state has not changed. 

<br>

#### **Interrupt Handlers**

`CAN_RX_ISR()` handles incoming CAN frames.

The interrupt handler retrieves the CAN message from the hardware and places the message into the receive queue.

Using a queue allows message processing to occur later in a task context rather than inside the interrupt handler.

<br>

`CAN_TX_ISR()` signals that a CAN transmission has completed.

The ISR releases the transmit semaphore so that the next message can be sent.

<br>

#### **FreeRTOS Tasks**

`CAN_TX_Task()`  is responsible for transmitting outgoing messages.

The task performs the following loop:

1. Wait for a message in the transmit queue.
2. Wait for a transmit semaphore.
3. Send the CAN frame.

Separating transmission into a task prevents application logic from blocking while waiting for CAN hardware availability. 

<br>

`decodeTask()`

Processes messages received from other synthesisers. The task:

1. Waits for a message in the receive queue.
2. Decodes the message into a `NoteMessage`.
3. Converts the message into synthesiser commands.

Depending on the synthesiser role, the message may trigger `synth.pushNoteOn()` or `synth.pushNoteOff()` .

The task also updates the shared system state with the received message for UI display.

<br>

#### **Runtime Optimisation**

CAN reception occurs in an interrupt handler. Only minimal work is performed at this level, all heavier processing occurs in a task context.

Queues decouple interrupt context, message processing, and synthesis commands, which prevents bursts of network traffic from blocking time-critical tasks.

The networking subsystem only generates messages when key state transitions occur. This significantly reduces the number of transmitted messages.

CAN messages use a fixed 8-byte format. This simplifies encoding and avoids dynamic memory allocation.

### User Interface Subsystem `src/ui`

#### **`Display.cpp`**

`updateState()` reads knob values and `synth.activeNotesBitmask` (an atomic ARM Cortex-M4 word read) and writes them into `sysState.displayState` under mutex protection. The octave knob index is selected based on the current `OctaveControlMode`.

`update()` snapshots display state from `sysState` under the mutex and calls `sendBuffer()` only if `stateChanged()` returns true, avoiding redundant I2C transfers. When `DINO_MODE` is defined it delegates entirely to `dinoGame.render()` and returns immediately.

`u8x8_byte_rtos_hw_i2c()` is the custom U8g2 I2C callback active when `I2C_EXPANDER_KNOBS` is defined. It acquires `knobManager.i2cMutex` on transfer start and releases it on transfer end, preventing interleaving with `knobTask` reads on the shared I2C bus.

#### **`DinoGame.cpp`**

Sprite data is stored as static XBM byte arrays: two alternating run frames and a dead frame for the dinosaur, and a cactus obstacle. All sprites are rendered using `u8g2.drawXBMP()`.

`tick()` drives the state machine each frame. A rising-edge check on the joystick input (`joy == JOY_UP && prevJoy_ != JOY_UP`) gates state transitions, preventing repeated triggers from a held input.

`updatePhysics()` applies gravity to `velY_` each frame and clamps the dinosaur at `GROUND_Y - 12`. `updateObstacle()` scrolls the cactus left by `speed_`, wrapping it to the right edge on exit and incrementing speed every ten obstacles. Collision uses AABB with a 2-pixel forgiveness margin on each side.

`drawDino()` selects the sprite based on state: dead frame when dead, `dino_run1` when airborne, and alternating between run frames every four ticks via `(frameCount_ / 4) % 2` when on the ground.

## Hardware Control `Knob.cpp`

The `Knob` class implements the behaviour of a single rotary encoder with an optional push-button input. Each instance of the class maintains the current encoder value, tracks quadrature state transitions, and detects button press events.

One instance of the `Knob` class represents an individual rotary control and maintains the following state:

- Current rotation value.
- Minimum and maximum allowed values.
- Previous quadrature state.
- Last detected rotation direction.
- Button press status.
- An atomic copy of the rotation value for concurrent access.

Using a separate atomic value allows the knob state to be safely accessed by multiple tasks without requiring mutex locking.

<br>

`Knob(int8_t startVal, int8_t min, int8_t max)`

Initialises the encoder state.

The constructor:

- Sets the starting rotation value.
- Defines the allowed range of motion.
- Initialises the previous encoder state.
- Initialises the atomic rotation value used by concurrent tasks.

This ensures the knob begins with a valid state and prevents unexpected transitions during initialisation.

<br>

`setInitialState(uint8_t currA, uint8_t currB)`

Initialises the encoder state using the current quadrature signals. The method stores the initial encoder state so that subsequent calls to `updateRotation()` correctly interpret the first detected transition. This prevents false rotation events when the system starts or when the encoder state is first read.

<br>

`updateRotation(uint8_t currA, uint8_t currB)`

Updates the encoder rotation value based on the current quadrature state.

The method:

1. reconstructs the two-bit encoder state.
2. Compares the current state with the previous state.
3. Determines the direction of rotation.
4. Updates the rotation value.
5. Clamps the value within configured bounds.

The algorithm includes logic to handle ambiguous transitions by using the previously detected direction of movement.

<br>

#### **Runtime Optimisations**

Encoder transitions are evaluated using simple comparisons and bit operations. This keeps the execution path short and predictable.

Rotation values and button press states are stored using atomic operations. This allows tasks and interrupt handlers to safely access the knob state without using mutex locks. Avoiding mutexes reduces context-switch overhead and prevents unnecessary blocking within time-critical tasks.

Rotary value clamping prevents invalid parameter values and avoids additional checks elsewhere in the system. 

Button presses are processed using an atomic exchange operation to clear the event flag when the press is consumed. This prevents duplicate processing of the same event.

Value retrieval uses an atomic load operation to ensure safe access without locking.