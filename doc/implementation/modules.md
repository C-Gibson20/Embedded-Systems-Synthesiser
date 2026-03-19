# 2. Module Documentation
[Return to Table of Contents](README.md)

This document describes the detailed implementation of the synthesiser system.  

## Source Code `src/`

The `src` directory contains the main synthesiser implementation. 
It is divided into subsystem directories reflecting the system architecture.

### Audio Subsystem `src/audio`

#### **`Synth.h` and `Synth.cpp`**

TODO Kayvan

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

#### **`Display.h`**

TODO Jeremy

#### **`Display.cpp`**

TODO Jeremy

#### **`DinoGame.h`**

TODO Jeremy

#### **`DinoGame.cpp`**

TODO Jeremy

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