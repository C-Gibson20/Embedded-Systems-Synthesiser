# Real-Time Music Synthesiser

Embedded Systems Coursework

---

## Overview

This project implements a real-time embedded music synthesiser on an STM32 microcontroller platform. The system generates audio waveforms in response to user input, provides real-time parameter control via rotary encoders, displays system state on an OLED interface, and supports distributed operation over a CAN bus.

The design emphasises deterministic timing, modular architecture, and safe concurrent execution using interrupts, DMA, and FreeRTOS-based scheduling.

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

<video src="doc/CAN.mp4" controls width="600"></video>

**Instrument Presets and Advanced Waveforms**

<video src="doc/Instruments.mp4" controls width="600"></video>

**Held Notes and Polyphony**

<video src="doc/Held.mp4" controls width="600"></video>

**Display-Based Mini Game**

<video src="doc/Dino.mp4" controls width="600"></video>
---