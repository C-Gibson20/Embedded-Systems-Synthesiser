# 6. Shared Resources and Synchronisation
[Back to Table of Contents](README.md) 

Multiple concurrent tasks interact within the system and therefore require coordinated access to shared resources. Without appropriate synchronisation, concurrent access could lead to race conditions, inconsistent system state, or data corruption.

The system uses a combination of FreeRTOS synchronisation primitives and atomic operations to ensure safe access to shared data structures.

## 6.1 Shared Data Structures

The shared resources used by the system are summarised in Table 1.

| Resource | Shared by | Access type | Protection |
| --- | --- | --- | --- |
| `sysState` | `scanKeysTask`, `decodeTask`, `handleSwitches`, `handleKeyChange`, `knobTask`, `Display` | Read/write | `sysState.mutex` |
| `msgInQ` | `CAN_RX_ISR`, `decodeTask` | Producer/consumer | FreeRTOS queue |
| `msgOutQ` | `scanKeysTask` via `handleKeyChange`, `CAN_TX_Task` | Producer/consumer | FreeRTOS queue |
| `txSemaphore` | `CAN_TX_ISR`, `CAN_TX_Task` | Signalling | Counting semaphore |
| `sampleBufferSemaphore` | DMA audio ISR, `sampleGenTask` | Signalling | Semaphore |
| Knob rotation / button state | `knobISR` and `knobTask`or `scanKeysTask`, `Knob`, `Display` | Read/write | Atomic operations |
| `i2cMutex`<br>(I2C expander mode only) | `knobTask`, `Display` I2C callback | Exclusive peripheral access | Mutex |

These resources represent shared system state and communication channels between concurrent tasks and interrupt contexts.

## 6.2 Synchronisation Mechanisms

Several synchronisation mechanisms are used to coordinate access to shared resources and communication between tasks. Each shared resource is protected using a mechanism appropriate to its access pattern, ensuring both safety and efficiency.

### Mutexes

Mutexes are used to protect shared system state that may be accessed by multiple tasks concurrently. Tasks must acquire the mutex before modifying the protected data structure.

Example:

```cpp
xSemaphoreTake(sysState.mutex, portMAX_DELAY);
```

FreeRTOS mutexes implement priority inheritance, which reduces the risk of priority inversion when higher-priority tasks are blocked by lower-priority tasks holding the mutex.

### Queues

Queues provide a thread-safe mechanism for transferring data between tasks. They are used to decouple producer and consumer tasks, allowing asynchronous communication without requiring shared memory access.

Examples:

- CAN transmit queue
- CAN receive queue
- Synthesiser command queue

Queues eliminate the need for explicit locking in many cases because access to the queue is internally synchronised by the FreeRTOS kernel.

### Semaphores

Binary and counting semaphores are used for signalling events between interrupt service routines and tasks.

Example:

```cpp
xSemaphoreGiveFromISR(...)
```

This mechanism allows interrupt handlers to notify tasks of hardware events without performing lengthy processing within the interrupt context.

### Atomic Operations

Atomic updates are used for certain small shared variables, such as encoder state. These operations avoid race conditions without requiring full mutex protection when the update can be performed in a single machine instruction.

## 6.3 Resource Contention and Blocking

Resource sharing introduces the possibility of blocking when tasks compete for exclusive access to protected resources. Blocking must be bounded to ensure that higher-priority tasks can still meet their deadlines.

A task may experience blocking when it attempts to acquire a resource currently held by a lower-priority task. The maximum blocking time $B_i$ for a task is determined by the longest critical section of lower-priority tasks that access the same resource.

$$
B_i = \max_{k \in LP(i)} (C_{k,cs})
$$

The value of $B_i$ is determined by identifying all mutexes accessed by task $i$ and selecting the longest critical section executed by any lower-priority task on those mutexes.

Estimated blocking times:

| Task | Blocking Source | $B_i$ |
| --- | --- | --- |
| `sampleGenTask` | None<br>Queue and semaphore usage only. | 0 |
| `CAN_TX_Task` | None<br>Queue and semaphore usage only. | 0 |
| `scanKeysTask` | `sysState.mutex`<br>Short critical section consisting of copy operations. | Negligible relative to $C_i$ |
| `decodeTask` | `sysState.mutex`<br>Short critical section consisting of copy operations. | Negligible relative to $C_i$ |
| `knobTask` | `sysState.mutex` and `i2cMutex` <br>Short critical sections consisting of copy operations. | Negligible relative to $C_i$ |
| `displayUpdateTask` | `sysState.mutex` and `i2cMutex` <br>Short critical sections consisting of copy operations. | Negligible relative to $C_i$ |
| `dinoTask` | None<br>No shared resource access. | 0 |

There are only two mutexes that may contribute to blocking in the system: `sysState.mutex` and `i2cMutex` (when I2C expander mode is enabled). Queues and semaphores do not contribute to $B_i$ in fixed-priority blocking analysis, as they cause task suspension rather than mutual exclusion blocking.

All critical sections are intentionally minimal, consisting only of copying shared data into local variables or writing updated values back to shared state. No complex computation or I/O operations are performed while holding a mutex. Consequently, blocking times are on the order of a few microseconds and are tightly bounded. Therefore blocking times $B_i$ are negligible compared to task execution times for all tasks and do not affect schedulability or response-time bounds.

## 6.4 Priority Inversion

Priority inversion occurs when a high-priority task is indirectly delayed by a lower-priority task holding a shared resource.

The system mitigates this effect through the use of FreeRTOS mutexes, which implement priority inheritance. When a lower-priority task holds a mutex required by a higher-priority task, the scheduler temporarily raises the priority of the mutex holder. This ensures that the resource is released as quickly as possible and prevents prolonged blocking of higher-priority tasks.

## 6.5 Deadlock Analysis

Deadlock requires a circular wait condition in which tasks hold one resource while waiting for another. In the present system, all mutex acquisitions are non-nested: no task holds one mutex while attempting to acquire a second.

Although cyclic communication paths exist via queues, these do not introduce deadlock as queues do not impose mutual exclusion constraints.

The two mutexes used (`sysState.mutex` and `i2cMutex`) are always acquired and released independently. As a result, the resource allocation graph contains no edges between mutexes and is therefore acyclic.

Since circular wait is a necessary condition for deadlock and cannot occur in this system, deadlock is provably impossible.