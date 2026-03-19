# 8. Conclusion
[Back to Table of Contents](README.md)

This project presented the design and analysis of a real-time music synthesiser implemented on an STM32 microcontroller platform. The system integrates audio synthesis, user input processing, display updates, and network communication within a concurrent FreeRTOS-based architecture.

Detailed timing analysis was performed to characterise the execution behaviour of each task, including worst-case execution time and minimum initiation intervals. These parameters were used to evaluate schedulability under a Rate Monotonic Scheduling (RMS) model. Critical instant analysis and utilisation bounds confirm that all tasks meet their deadlines under worst-case conditions.

The system architecture combines interrupt-driven hardware interaction with thread-based task processing, enabling deterministic audio generation while maintaining responsive user interaction and communication capabilities. The modular subsystem design and use of synchronisation mechanism allowed extension of the system without comprising real-time performance guarantees.