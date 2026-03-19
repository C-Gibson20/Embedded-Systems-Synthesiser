# 3. Task Identification
[Back to Table of Contents](README.md)

The synthesiser operates as a concurrent real-time system composed of multiple FreeRTOS threads and interrupt service routines (ISRs). Threads implement higher-level processing tasks, while interrupts handle time-critical hardware events such as audio buffer transfers ad peripheral communication.

All threads are created during system initialisation and execute under the FreeRTOS scheduler. Interrupt handlers are registered with the relevant hardware peripherals and signal threads when asynchronous events occur.

## 3.1 Thread-Based Tasks

The primary application functionality is implemented using FreeRTOS threads. Each thread is responsible for a specific functionality within the synthesiser.

| Task | Function |
| --- | --- |
| `sampleGenTask` | Generate audio samples for the synthesis engine |
| `scanKeysTask` | Periodically scans the keyboard matrix and detects key events |
| `decodeTask` | Process received CAN messages |
| `CAN_TX_Task` | Transmits queued CAN messages |
| `displayUpdateTask` | Update the OLED display with system status |
| `knobTask` | Process rotary encoder input events |
| `dinoTask` | Executes the optional game loop, updating game state and rendering frames to the display |

These threads operate concurrently and communicate using queues, semaphores, and shared system state.

## 3.2 Interrupt Handlers

Several time-critical operations are handled using ISRs. Interrupts allow the system to respond immediately to hardware events while deferring longer processing operations to threads.

| Interrupt | Purpose | Worst-Case Frequency |
| --- | --- | --- |
| DAC DMA interrupt | Signals half/full completion of the audio buffer transfer | $f_{DMA}=\frac{2f_s}{B}=\frac{2\cdot22000}{128}\approx344Hz$ |
| CAN RX interrupt | Captures incoming CAN messages | $f_{CAN}=\frac{\text{bitrate}}{\text{bits per frame}}=\frac{1 M}{120}\approx 8333Hz$  |
| CAN TX interrupt | Indicates completion of CAN transmission | $f_{CAN}=\frac{\text{bitrate}}{\text{bits per frame}}=\frac{1 M}{120}\approx 8333Hz$ |
| Encoder knob interrupt<br>(I2C expander mode only) | Detects rotary encoder state changes | 4 transitions per detent with estimated 50 detents per second for very fast spin. <br>$f_{encoder}\approx200Hz$<br>(Estimated from video footage) |

Although the theoretical CAN interrupt rate is bounded by bus bandwidth, the actual system load is significantly lower as message generation is limited by user input rate.

In the matrix-scanned configuration, rotary encoders are processed without interrupts and therefore contribute no ISR load. In configurations using an I2C expander, the interrupt rate is bounded by the maximum quadrature transition rate of the encoders. Based on physical interaction limits, this is on the order of tens to hundreds of events per second, which is negligible compared to other system interrupts.

The DAC DMA interrupt is particularly important for maintaining deterministic audio output. When half or the entire audio buffer has been transmitted, the interrupt signals the audio generation thread to produce the next block of samples.

## 3.3 Task Priority Assignment

| Task | Priority | Frequency | Event Driven |
| --- | --- | --- | --- |
| `sampleGenTask` | Highest 5 | $f_{DMA}=344Hz$ | Yes<br>(Periodic) |
| `CAN_TX_Task` | High 4 | $f_{CAN\_TX}=12\times f_{scan}=600Hz$ | Yes |
| `scanKeysTask` | Medium 3 | $f_{scan}=50Hz$ | No |
| `decodeTask` | Medium 2 | $f_{decode}=f_{CAN}=8333Hz$ | Yes |
| `knobTask`<br>I2C expander mode only | Medium 2 | $f_{knob}=f_{encoder}=200Hz$ | Yes |
| `dinoTask` | Low 1 | $f_{dino}=60 Hz$ | No |
| `displayUpdateTask` | Low 1 | $f_{display}=10 Hz$ | No |

Event-driven tasks are assigned an equivalent worst-case frequency based on the maximum rate at which triggering events can occur. For CAN-related tasks, this is bounded by the maximum CAN frame rate and system-level constraints such as key scan frequency and queue capacity.

Although RMS assigns priorities based on task frequency, practical real-time systems must also consider task criticality. The `sampleGenTask` is assigned the highest priority because it is responsible for maintaining deterministic audio output, which is a hard real-time requirement.

Event-driven tasks such as `CAN_TX_Task` and `decodeTask` may exhibit higher theoretical activation frequencies. However, these tasks are buffered through queues and do not directly impact time-critical audio generation. Therefore, they are assigned lower priorities to ensure that communication processing does not interfere with audio deadlines.

## 3.4 Justification of Interrupt and Thread Usage

Interrupts are used for operations requiring immediate response to hardware events. In particular, DMA transfer interrupts are used to trigger audio buffer updates, ensuring that audio samples are generated before the DAC requires new data.

Similarly, CAN receive interrupts capture incoming messages with minimal latency and enqueue them for later processing by a thread.

Longer processing tasks, such as keyboard scanning, display updates, and message decoding, are implemented as threads. This prevents excessive work from occurring inside interrupt handlers and ensures predictable system scheduling.