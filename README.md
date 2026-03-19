# Real-Time Music Synthesiser

Embedded Systems Coursework

---

## Overview

This project implements a real-time embedded music synthesiser on an STM32 microcontroller platform. The system generates audio waveforms in response to user input, provides real-time parameter control via rotary encoders, displays system state on an OLED interface, and supports distributed operation over a CAN bus.

The design emphasises deterministic timing, modular architecture, and safe concurrent execution using interrupts, DMA, and FreeRTOS-based scheduling.

---

## Demonstration

A demonstration of the system, including all implemented features, is shown below:

<!-- Replace with actual video link -->
[![System Demonstration]()]()

The demonstration includes:

- Real-time multi-voice audio synthesis.
- Keyboard input and note triggering.
- Rotary encoder parameter control.  
- OLED display updates.
- CAN-based communication between devices.  
- Optional display-based game mode.  

---

## Documentation

The project documentation is organised into two main sections:

### System Design and Analysis

- [Report Documentation](docs/report/README.md)

Covers system architecture, task design, timing analysis, schedulability, and resource management.

### Implementation Documentation

- [Implementation Documentation](docs/implementation/README.md)

Describes the codebase structure, module responsibilities, and implementation details.

---

## Repository Structure

```text
docs/
├── report/           # Design, analysis, and real-time evaluation
└── implementation/   # Code structure and implementation details

src/                  # Source code
include/              # Shared headers
lib/                  # External libraries
scripts/              # Development utilities