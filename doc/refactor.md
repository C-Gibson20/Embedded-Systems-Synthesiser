# Refactor Notes

## Easy / Low Risk

### Dead commented-out code
- `src/audio/Synth.cpp:109, 228-232` — old `HardwareTimer`/`sampleTimer_` implementation, replaced by DMA
- `src/config.cpp:18-31` — `#if 0` block with unused alternative oscillator config (MSI + LSE)
- `src/main.cpp:55` — debug comment `// role = RECEIVER; // Force receiver for testing`

### Unused declaration
- `include/Knob.h:31` — `getValueISR()` declared but never defined or called anywhere

---

## Medium

### Unused DinoGame stubs
- `src/ui/DinoGame.h:39-42` — 4 no-arg stub method declarations (`drawDino`, `drawObstacle`, `drawGround`, `drawScore`) that are never called
- `src/ui/DinoGame.cpp:190-193` — corresponding stub implementations

### Unused AudioCommand fields
- `src/audio/Synth.h:23-27` — `updatePitch`, `updateVolume`, `updateWave`, `updateOctave`, `octaveValue` fields in `AudioCommand` struct are never populated or read

---

## Needs Care

### Data race on `sounds[]`
- `src/ui/Display.cpp:65` — TODO comment acknowledges `sounds[]` is read in `displayUpdateTask` while written by `sampleGenTask` without synchronisation
- Removing `volatile` from `sounds[]` improved WCET by ~33% but did not fix the underlying race
- Fix: take `sysState.mutex` (or a dedicated mutex) around the `sounds[]` read in `Display::updateState()`, or have `sampleGenTask` write a snapshot that `displayUpdateTask` reads
