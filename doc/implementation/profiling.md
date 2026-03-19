# 3. Profiling
[Return to Table of Contents](README.md)

This document describes the profiling techniques used to measure execution times within the synthesiser system.

Profiling was performed to estimate the worst-case execution time (WCET) of tasks and interrupt handlers used in the schedulability analysis presented in the report.

## 3.1 Profiling Objectives

Profiling was conducted to measure the execution time of the following components:

- FreeRTOS task functions.
- Interrupt service routines (ISRs).

The goal was to obtain conservative estimates of the WCET $C_i$ for each task.

## 3.2 Profiling Mode

A dedicated profiling configuration is implemented to measure WCET for all tasks and interrupt service routines.

When `PROFILING_MODE` is enabled, the system is modified to isolate execution paths and remove scheduling interference:

- All FreeRTOS threads are disabled using compile-time flags.
- Interrupt-driven behaviour can be selectively disabled.
- Individual tasks and ISRs are executed in isolation through direct function calls.

This configuration ensures that each component can be measured deterministically without interference from concurrent execution.

### Measurement Procedure

Execution time is measured using the `micros()` timer, which provides microsecond-resolution timestamps. For each task or ISR:

1. The system is configured to enable profiling for the target component.
2. The component is executed repeatedly for a 32 iterations.
3. Each execution is timed using:
    - Start timestamp before execution.
    - End timestamp immediately after execution.
4. The total execution time is accumulated and averaged to obtain a representative WCET estimate.

The profiling functions used are:

```cpp
uint32_t start = micros();
taskFunction(NULL);
uint32_t end = micros();
```

and

```cpp
uint32_t start = micros();
isrFunction();
uint32_t end = micros();
```

To improve measurement stability:

- Multiple iterations are used to reduce measurement noise.
- Optional delays are inserted between iterations for hardware-dependent tasks.
- Queues and semaphores are reset where necessary to avoid saturation effects.

This approach provides repeatable and conservative execution time measurements suitable for real-time analysis.

## 3.3 Profiling Methods

Each task and ISR is profiled under conditions designed to approximate worst-case execution behaviour. This is achieved by forcing execution paths that maximise computational workload and resource usage.

### Task Profiling

Tasks are profiled by directly invoking their entry functions using the `profileTask()` utility. During profiling, normal task scheduling is disabled, and each task is executed sequentially.

Worst-case behaviour is enforced through controlled input conditions:

- Audio generation (`sampleGenTask`)
All voices are activated with the most computationally expensive waveform. This ensures maximum polyphony and worst-case synthesis workload.
- Keyboard scanning (`scanKeysTask`)
All keys are forced to change state simultaneously, and the system is configured in receiver mode to trigger both local synthesis and CAN message handling paths.
- Display update (`displayUpdateTask`)
Rendering paths are forced to produce maximum output, ensuring worst-case pixel processing and I2C transmission.
- CAN decoding (`decodeTask`)
Input queues are reset to ensure continuous processing of incoming messages without early exit conditions.
- CAN transmission (`CAN_TX_Task`)
Delays are introduced between iterations to allow hardware transmission completion and ensure realistic timing behaviour.

### ISR Profiling

ISRs are profiled using the `profileISR()` function, which invokes ISR handlers directly.

Special handling is applied where required:

- CAN queues and semaphores are reset prior to profiling to ensure consistent execution conditions.
- Binary semaphores are cleared before invocation to avoid missed signal events.
- ISR execution is measured without scheduling interference, providing a direct estimate of handler execution time.

It is noted that the WCET approach may overestimate typical runtime behaviour. However, such conservatism is appropriate for real-time system design, where guarantees must hold under worst-case conditions.

## 3.4 Measurement Limitations

The profiling approach measures the computational execution time of task and interrupt functions.

However, the following factors are not directly captured:

- Scheduler overhead.
- Context switching latency.
- Task wake-up delays caused by the FreeRTOS scheduler tick.

Overcoming this has been discussed in the report [scheduling_analysis.md](report/scheduling_analysis.md).