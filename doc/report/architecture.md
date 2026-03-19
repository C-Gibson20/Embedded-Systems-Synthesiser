# 1. System Architecture

[Back to Table of Contents](README.md)

The synthesiser system is implemented as a real-time embedded application running on an STM32L4 microcontroller. The architecture integrate hardware peripherals, interrupt-driven mechanisms, and FreeRTOS-based task scheduling to support deterministic audio generation while concurrently handling user input, display updates, and network communication.

The system architecture can be divided into two main components, the hardware platform and the software subsystem structure.

## 1.1 Hardware Architecture

The synthesiser is implemented on an STM32L4 microcontroller interfaced with a StackSynth hardware module providing audio output, input controls, and display functionality.

The principal hardware components used by the system are shows in Table 1.

| Component | Function |
| --- | --- |
| DAC | Digital-to-analogue conversion for audio output. |
| Timer 6 | Generates periodic audio sample timing |
| DMA | Transfers audio buffers to the DAC |
| Key matrix | Musical keyboard input |
| Rotary encoders | Parameter adjustment |
| OLED display | User interface output |
| CAN controller | Inter-device communication |

Audio samples are produced by the synthesis engine and written to a memory buffer. A DMA controller continuously transfers this buffer to the DAC, while Timer 6 provides the periodic trigger required for consistent audio sample timing. This hardware-assisted pipeline ensures that audio output occurs at a fixed sample rate while minimising CPU overhead.

User interaction is provided through a scanned key matric and rotary encoders while visual feedback is presented on an OLED display. CAN communication 

## 1.2 Software Architecture

The software system is structured into modular subsystems that separate functional responsibilities and simplify concurrent execution. Table 2 summarises the main subsystems.

| Subsystem | Responsibility |
| --- | --- |
| Audio | Voice management and waveform synthesis |
| Input | Keyboard scanning and rotary encoder decoding |
| Networking | CAN message transmission and reception |
| UI | OLED display updates |
| System state | Shared synthesis parameters and configuration |

The audio subsystem is responsible for generating waveform samples for all active voiced during each audio update cycle. Input subsystems detect key pressed and control changes, while the networking subsystem manages CAN message exchange between devices. The user interface subsystem updates the display to reflect the current system state and parameters.

Communication between subsystems is implemented using FreeRTOS synchronisation primitives and message passing mechanisms. These include:

- FreeRTOS queues for inter-task communication.
- Mutex-protected shared state for coordinated access to global parameters.
- Command buffers for transferring control events between tasks.

This architecture allows independent subsystems to operate concurrently while maintaining deterministic behaviour and safe access to shared resources.

Detailed documentation of the codebase, including directory organisation, module responsibilities, and implementation details, is provided in the repository's implementation documentation:

[Implementation and Code Structure](../implementation/code_structure.md)