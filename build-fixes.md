# Build Fixes

---

## 1. `constants.h` — refactor.md compliance issues

### Multiple Definition Bugs
`extern const` with an initialiser in a header causes linker errors when included in more than one `.cpp`. Affects:
- `displayInterval`, `pitchIdx`, `waveIdx`, `octaveIdx`, `volumeIdx`, `octaveOffsetIdx`, `MAX_SOUNDS`

**Fix:** Change to `constexpr` (or `inline constexpr` if needed).

### Wrong Constant Names (§2, §11 of refactor.md)
| Current | Required |
|---|---|
| `fs` | `SAMPLING_RATE` (`constexpr double`) |
| `pow2_32` | `PHASE_MODULUS` (`constexpr uint64_t = 1ULL << 32`) |
| `MAX_SOUNDS` | `MAX_VOICES` (`constexpr uint8_t`) |
| `displayInterval` | `DISPLAY_INTERVAL` |
| `scanInterval` | `SCAN_INTERVAL` |
| `octave` | `DEFAULT_OCTAVE` (or remove if unused) |
| `noteNames[]` | `NOTE_NAMES` |
| `waveNames[]` | `WAVE_NAMES` |
| `f_notes[]` | `F_NOTES` |
| `stepSizes[]` | `STEP_SIZES` |

### Knob Indices (§2 of refactor.md)
Replace the five separate `extern const uint8_t` index variables with:
```cpp
enum class KnobIndex : uint8_t { PITCH = 0, WAVEFORM, VOLUME, OCTAVE, MODE };
```

### Stray Include
`#include <bitset>` does not belong in `constants.h` — remove it.

---

## 2. `pins.h` — refactor.md compliance issues

### Object Definitions in a Header (Multiple Definition Bug)
`pins.h` defines objects, not just constants. Including it in more than one `.cpp` will cause linker errors. Move each to its owning module:

| Definition | Move to |
|---|---|
| `U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);` | `src/ui/Display.cpp` |
| `HardwareTimer sampleTimer(TIM1);` | `src/audio/Synth.cpp` |
| `QueueHandle_t msgInQ;` / `msgOutQ;` | `src/net/CanProtocol.cpp` or `SysState` |

Per `refactor.md` §1, `pins.h` should contain **only** pin definitions and mux bit constants.

### `extern const` Bug
`extern const int DEN_BIT = 3;` has the same multiple-definition issue as above.

**Fix:** Change to `constexpr int DEN_BIT = 3;`
