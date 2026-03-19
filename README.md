# Real-Time Music Synthesiser

Embedded Systems Coursework

---

## Overview

This project implements a real-time embedded music synthesiser on an STM32 microcontroller platform. The system generates audio waveforms in response to user input, provides real-time parameter control via rotary encoders, displays system state on an OLED interface, and supports distributed operation over a CAN bus.

The design emphasises deterministic timing, modular architecture, and safe concurrent execution using interrupts, DMA, and FreeRTOS-based scheduling.

---

## Documentation

The project documentation is organised into two main sections:

### System Design and Analysis

- [Report Documentation](doc/report/README.md)

Covers system architecture, task design, timing analysis, schedulability, and resource management.

### Implementation Documentation

- [Implementation Documentation](doc/implementation/README.md)

Describes the codebase structure, module responsibilities, and implementation details.

---

## Repository Structure

```text
doc/
├── report/           # Design, analysis, and real-time evaluation
└── implementation/   # Code structure and implementation details

src/                  # Source code
include/              # Shared headers
lib/                  # External libraries
scripts/              # Development utilities
```
---

## Demonstrations

**CAN-Based Distributed Synthesis**

https://github.com/user-attachments/assets/94364843-ae49-44b5-b197-1cc3da56e4ed
<!-- <video src="doc/CAN.mp4" controls width="600"></video> -->
[CAN-Based Distributed Synthesis Video](doc/CAN.mp4)

**Instrument Presets and Advanced Waveforms**

https://github.com/user-attachments/assets/65f04ced-008e-4458-b12b-2c6a1a11a4ac
<!-- <video src="doc/Instruments.mp4" controls width="600"></video> -->
[Instrument Presets and Advanced Waveforms Video](doc/Instruments.mp4)

**Held Notes and Polyphony**

https://github.com/user-attachments/assets/5124a1bb-8d5a-4f10-9eff-a028388088a4
<!-- <video src="doc/Held.mp4" controls width="600"></video> -->
[Held Notes and Polyphony Video](doc/Held.mp4)

**Display-Based Mini Game**

https://github.com/user-attachments/assets/3525706e-4e00-4314-abe7-5119c1b3943a
<!-- <video src="doc/Dino.mp4" controls width="600"></video> -->
[Display-Based Mini Game Video](doc/Dino.mp4)
