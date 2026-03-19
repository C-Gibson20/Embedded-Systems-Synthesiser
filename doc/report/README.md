# Real-Time Music Synthesiser Report

This project implements a real-time embedded music synthesiser using an STM32 microcontroller platform. The system generates digital audio waveforms in response to keyboard input, provides parameter control via rotary encoders, displays system status on an OLED interface, and supports communication with other synthesisers over a CAN bus.

Real-time audio synthesis imposes strict timing constraints. Audio samples must be produced at deterministic intervals to prevent audible artefacts such as jitter, distortion, or buffer underruns. Consequently, the system must guarantee predictable execution and bounded latency across concurrent tasks.

To satisfy these requirements, the system combines several embedded real-time mechanisms:

- Hardware interrupts for time-critical events.
- Direct Memory Access (DMA) for continuous audio sample transfer.
- FreeRTOS threads for concurrent task execution.

Task scheduling follows the Rate Monotonic Scheduling (RMS) model, in which tasks with shorter periods are assigned higher priorities. This approach enables deterministic scheduling analysis and allows verification that all tasks meet their deadlines under worst-case execution conditions.

The synthesiser support multi-voice waveform generation, matrix-scanned keyboard input, rotary encoder parameter control, OLED-based user feedback, and inter-device communication over CAN. These components operate concurrently within the real-time system while maintaining safe access to shared resources.

This document presents the design and real-time analysis of the synthesiser system. It identifies the tasks implemented within the system, characterises their timing behaviour, and evaluates schedulability under a rate monotonic scheduling model. Resource sharing, blocking behaviour, and potential deadlock conditions are also analysed.

This documentation is organised according to the table of contents below.

## Table of Contents

[Return to root README](../../README.md)

### System Overview

1. [System Architecture](architecture.md)

2. [Audio Synthesis System](synthesis.md)

### Real-Time Analysis

3. [Task Identification](tasks.md)

4. [Task Timing Characterisation](timing_analysis.md)

5. [Scheduling Analysis](scheduling_analysis.md)

### Resource Management

6. [Shared Resources and Synchronisation](resources.md)

### System Features

7. [CAN Communication](communication.md)

### Conclusion

8. [Conclusion](conclusion.md)