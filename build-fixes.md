# Build Fixes

## Status: Linking stage — all compile errors resolved

---

## 1. `const` internal linkage — add `extern` to definitions in `main.cpp`

`const` variables in C++ have internal linkage by default, so `extern const` in
`Display.cpp` can't find them at link time. Add `extern` to their definitions in `main.cpp`:

```cpp
extern const uint8_t pitchIdx = 0;
extern const uint8_t waveIdx = 1;
extern const uint8_t octaveIdx = 2;
extern const uint8_t volumeIdx = 3;
extern const uint8_t octaveOffsetIdx = 4;
extern const int MAX_SOUNDS = 16;
extern const uint32_t displayInterval = 100;
extern const int DRST_BIT = 4;
extern const int DEN_BIT = 3;
```

---

## 2. `setOutMuxBit` signature mismatch — fix extern in `Display.cpp`

Actual signature in `main.cpp`:
```cpp
void setOutMuxBit(const uint8_t bitIdx, const bool value)
```

Current extern in `Display.cpp` uses `int` instead of `const uint8_t` — linker sees
them as different symbols. Change the extern declaration in `Display.cpp` to:
```cpp
extern void setOutMuxBit(const uint8_t bitIdx, const bool value);
```
