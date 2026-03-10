# Refactoring Plan

## Overview

`main.cpp` is 1514 lines and mixes ISR handlers, FreeRTOS tasks, audio synthesis, key scanning,
CAN communication, display rendering, and hardware initialization. This plan breaks it into
focused modules, eliminates pervasive magic numbers, and clarifies synchronization boundaries.

---

## 1. File Structure

**`include/` vs `src/`:** PlatformIO treats `include/` as the project-wide public
include path — use it for headers needed across multiple modules. `src/` headers are
private to their module. `SysState.h` belongs in `include/` since every module
depends on it. Module-specific headers (`Display.h`, `Synth.h`) stay in `src/`.

Target layout:

```
include/                          — project-wide public headers (auto on include path)
├── Knob.h                        (already here)
├── sine_lut.h                    (already here)
├── SysState.h                    — shared state, enums (SynthRole, SynthWaveform,
                                    OctaveControlMode, DisplayState, Sound)
├── constants.h                   — MAX_SOUNDS, SAMPLING_RATE, PHASE_MODULUS, etc.
└── pins.h                        — all pin definitions and mux bit constants
src/
├── main.cpp              (~150 lines — setup(), loop(), task registration only)
├── config.cpp            (keep as-is, clock config)
├── Knob.cpp              (keep, minor fixes)
├── audio/
│   ├── Synth.h / Synth.cpp       — sound allocation, waveform generation, ISR
│   └── AudioCommand.h            — AudioCommand struct, queue push/pop API
├── io/
│   ├── KeyMatrix.h / KeyMatrix.cpp — row/col scanning, bitset output
│   └── KnobManager.h / KnobManager.cpp — array of Knobs, unified read API
├── net/
│   ├── CanProtocol.h / CanProtocol.cpp — message struct, encode/decode, validation
│   └── (ES_CAN stays in lib/)
└── ui/
    └── Display.h / Display.cpp   — render functions, state-to-string helpers
```

**Current progress:**
- `SysState.h` created in `src/` (should move to `include/`) — holds enums
  (`SynthRole`, `SynthWaveform`, `OctaveControlMode`), `DisplayState`, `Sound`,
  and `SysState` struct with `extern SysState sysState`
- `constants.h` created in `include/` — all constants renamed to `UPPER_SNAKE_CASE`;
  `SAMPLING_RATE`, `PHASE_MODULUS`, `MAX_VOICES` per spec; `KnobIndex` enum class;
  `constexpr` throughout (no `extern const` bugs)
- `pins.h` created in `include/` — pure pin + mux bit constants only (`constexpr`);
  object definitions (`u8g2`, `sampleTimer`, `msgInQ/Out`) moved to owning modules
- `KeyMatrix` class extracted to `src/io/KeyMatrix.h` / `KeyMatrix.cpp` —
  GPIO macros, `selectRow`, `readColumns`, `scan()` (returns `KeyScanResult`),
  `scanRow()`, and `setOutMuxBit` all live here; `main.cpp` scan loop replaced
  with `matrix.scan()`
- `Display` class extracted to `src/ui/Display.h` / `Display.cpp` —
  `u8g2_` is a member; `begin()`, `update()`, `updateState()`, `stateChanged()`
  are methods; `display` global defined in `Display.cpp`; `displayUpdateTask`
  is a thin RTOS-loop wrapper around `display.update()`
- `main.cpp` cleaned of all commented-out blocks; down to ~900 lines

**Remaining in `main.cpp` to extract:**
- Move `SysState.h` from `src/` to `include/`

---

## 2. Eliminate Magic Numbers

All bare literals should become named constants in a central `constants.h` (or inside the
relevant module's header).

**Audio:**
```cpp
// Before
constexpr double SAMPLING_RATE = 22000.0;
constexpr uint32_t PHASE_MODULUS = 4294967296;   // 2^32

// After
constexpr double SAMPLING_RATE    = 22'000.0;
constexpr uint64_t PHASE_MODULUS  = 1ULL << 32;   // self-documenting
```

**Polyphony:**
```cpp
// Before: MAX_SOUNDS = 16 and invGain[] both hardcoded
constexpr uint8_t MAX_VOICES = 16;
// invGain computed from MAX_VOICES at compile time or as a constexpr function
```

**CAN bit timing:**
```cpp
constexpr uint32_t CAN_PRESCALER = 40;
constexpr uint32_t CAN_BS1       = CAN_BS1_13TQ;
constexpr uint32_t CAN_BS2       = CAN_BS2_2TQ;
constexpr uint32_t CAN_SJW       = CAN_SJW_2TQ;
```

**Pin definitions:** All pin constants should live in one `pins.h` with clear grouping
(row-select, column-read, audio-out, etc.).

**Knob indices:**
```cpp
enum class KnobIndex : uint8_t { PITCH = 0, WAVEFORM, VOLUME, OCTAVE, MODE };
```

---

## 3. CAN Message Protocol

The byte-array protocol is currently decoded by indexing magic numbers in multiple places.
Replace with a typed struct and explicit encode/decode functions in `net/CanProtocol.h`.

```cpp
// CanProtocol.h
enum class MsgType : uint8_t { NOTE_ON = 'P', NOTE_OFF = 'R' };

struct NoteMessage {
    MsgType   type;
    uint8_t   keyIdx;
    int8_t    pitch;
    Waveform  waveform;
    uint8_t   octave;
    uint8_t   volume;
};

NoteMessage decodeNoteMessage(const uint8_t raw[8]);
void        encodeNoteMessage(const NoteMessage&, uint8_t out[8]);
bool        validateNoteMessage(const NoteMessage&);  // range checks
```

`decodeTask` and `scanKeysTask` both encode/decode messages; centralizing removes duplication
and the risk of the two sides drifting out of sync.

---

## 4. Modularise `sampleISR` / Audio Synthesis

`sampleISR` (~61 lines) does allocation scanning, queue draining, phase stepping, waveform
computation, and mixing. Split into:

- `AudioCommand.h` — the command struct and a lock-free push/pop pair
- `Synth::processCommands()` — drain command queue, allocate/free voices
- `Synth::tick()` — called by ISR: advance phases, sum voices, return output sample
- `Synth::allocateVoice()` / `freeVoice()` — explicit LIFO allocator

The ISR becomes:
```cpp
void sampleISR() {
    synth.processCommands();
    int32_t sample = synth.tick();
    analogWrite(OUTR_PIN, sample >> 8);
}
```

This makes the ISR trivially auditable and lets synthesis logic be unit-tested off-target.

---

## 5. Modularise Key Matrix Scanning

Extract `scanKeysTask` logic into a `KeyMatrix` class that owns the row/column GPIO
manipulation and returns a `std::bitset<32>` of current key states.

```cpp
class KeyMatrix {
public:
    void        begin();
    std::bitset<32> scan();       // performs one full scan cycle
private:
    void        selectRow(uint8_t row);
    uint8_t     readColumns();
};
```

`scanKeysTask` then just calls `matrix.scan()`, diffs against previous state, and enqueues
the appropriate audio commands and CAN messages. The GPIO register manipulation (`GPIOA->BSRR`)
stays inside `KeyMatrix`, hidden from task code.

---

## 6. Replace `#ifdef` Version Switches with Runtime Configuration

Multiple code paths (`V1`, `V2`, `I2C_EXPANDER_KNOBS`, `PROFILING_MODE`, `DISABLE_THREADS`)
are selected by compile-time `#ifdef`. This means uncompiled paths decay silently.

**Preference:** Collapse V1/V2 knob differences into a strategy object or a small struct of
function pointers selected once during `setup()` based on a single compile-time flag (or
board detection). The duplicated knob-read logic (lines 697–727) becomes one call:
`knobManager.readAll()`, with the I2C vs. matrix back-end injected.

Profiling hooks can stay as `#ifdef PROFILING_MODE` but should be isolated to a single
`profiling.h` file rather than scattered inline.

---

## 7. Synchronization Cleanup

Every access to `sysState` repeats the same mutex take/give boilerplate:
```cpp
xSemaphoreTake(sysState.mutex, portMAX_DELAY);
// ... one-liner read ...
xSemaphoreGive(sysState.mutex);
```

Replace with RAII helper and accessor methods:
```cpp
// SysState.h
class SysState {
public:
    // Locked accessors — short critical sections only
    SynthRole   getRole() const;
    void        setRole(SynthRole);
    std::bitset<32> getInputs() const;
    // ...
private:
    SemaphoreHandle_t mutex_;
    // raw members private
};
```

This enforces that no code holds the mutex across blocking calls and makes all shared-state
accesses easy to grep.

---

## 8. Fix `Knob` Class

Current issues:
- `begin()` is empty but called.
- `getValueISR()` declared but never defined.
- `buttonWasPressed` is non-atomic (written in task, read elsewhere).

Changes:
- Remove or implement `begin()`.
- Remove `getValueISR()` or implement it using `__atomic_load_n`.
- Make `buttonWasPressed` an `std::atomic<bool>` or `volatile bool` with documented access rules.

---

## 9. Display Rendering

`displayUpdateTask` (~85 lines) mixes state reading, string formatting, and U8g2 draw calls.
Extract to `Display.cpp`:

```cpp
// Display.h
void renderHome(const DisplaySnapshot&);
void renderNoteActive(const DisplaySnapshot&);
// ...
```

`DisplaySnapshot` is a plain struct copied from `sysState` under the mutex. The render
functions operate only on the snapshot — no mutex needed inside them.

---

## 10. Error Handling

Minimum additions (not exhaustive):
- Check `Wire.endTransmission()` return; log or assert on failure.
- Check `xQueueSend()` return in `pushAudioCommand()`; drop-with-counter or assert.
- Validate CAN message fields in `validateNoteMessage()` before processing.
- `allocateVoice()` returning -1 should be visible (increment a saturated counter, at minimum).

---

## 11. Naming Conventions

Apply consistently:
- **Types / classes:** `PascalCase` — `SynthWaveform`, `AudioCommand`, `KeyMatrix`
- **Functions / methods:** `camelCase` — `processCommands()`, `scanKeys()`
- **Constants:** `UPPER_SNAKE_CASE` — `MAX_VOICES`, `SAMPLING_RATE`
- **Member variables:** trailing underscore — `mutex_`, `sounds_`
- **Macros (GPIO):** keep `UPPER_SNAKE_CASE`; prefer `inline` functions where C++ allows
- Remove single-letter loop variables where the meaning is non-obvious; use `voiceIdx`, `rowIdx`, etc.

---

## 12. Suggested Refactoring Order

Work in small, independently testable increments:

1. ~~**Extract `constants.h` and `pins.h`**~~ ✅ done
2. ~~**Extract `CanProtocol`**~~ ✅ done
3. ~~**Extract `KeyMatrix` class**~~ ✅ done
4. ~~**Extract `KnobManager`**~~ ✅ done
5. ~~**Extract `Synth` module**~~ ✅ done
6. ~~**Extract `Display` module**~~ ✅ done
7. **Refactor `SysState`** — RAII mutex wrappers last (highest churn)
8. **Fix `Knob` class** — small isolated changes

This order minimises the risk of introducing regressions: each step changes one concern and
can be verified on hardware before the next step begins.
