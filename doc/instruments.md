# Instrument Presets

## Overview

The six waveform slots (previously Square, Saw, Triangle, Sine, SuperSaw, SineFold) have been
replaced with four instrument presets. The knob range was reduced from 0–5 to 0–3 to match.

Each instrument preset defines:
- Which oscillator waveform to use underneath
- Its own ADSR envelope (attack, decay, sustain level, release)
- A loudness gain compensation value (to normalise perceived volume across instruments)
- A biquad low-pass filter (2nd-order Butterworth) tuned to the instrument's character

---

## Files Changed

- `include/SysState.h` — added `Instrument` enum (`PIANO, MIDI, VIOLIN, FLUTE`); added
  `EnvPhase` enum; added `envPhase`, `envLevel`, `attackRate`, `decayRate`, `sustainLevel`,
  `releaseRate`, `gain` fields to `Sound`; renamed `DisplayState.waveform` → `DisplayState.instrument`
- `include/constants.h` — replaced `WAVE_NAMES` with `INSTRUMENT_NAMES`
- `src/audio/Synth.h` — `AudioCommand` and `GlobalParameters` use `Instrument` instead of
  `SynthWaveform`; replaced `filterPrev_` with biquad state (`bqX1_, bqX2_, bqY1_, bqY2_`);
  removed `filterAlpha` from `GlobalParameters`
- `src/audio/Synth.cpp` — `InstrumentPreset` stores ADSR rates, gain, and biquad coefficients
  (Q1.14); `NOTE_ON` sets `envPhase = ENV_ATTACK`, `envLevel = 0`, and copies all preset fields
  to the voice; `NOTE_OFF` and `HOLD_OFF` transition to `ENV_RELEASE` instead of immediately
  deactivating; `applyGlobalParamUpdates` applies preset to active voices; `tick()` runs the ADSR
  state machine per voice, applies per-voice gain, and applies a mix-level biquad filter
- `src/ui/Display.cpp` — updated field name and string lookup to use `INSTRUMENT_NAMES`
- `src/io/KnobManager.cpp` — WAVEFORM knob range reduced from `0,5` to `0,3`

---

## ADSR Envelope

Each voice has an Attack-Decay-Sustain-Release amplitude envelope. Notes fade in, shape over
time, and fade out naturally instead of snapping on/off instantly. This removes click artefacts
and makes the synth sound more expressive.

The envelope is a 16-bit value (`envLevel`, 0–65535) updated every sample in `tick()`. Rates
are stored per-voice and set from the instrument preset at note-on time:

```
rate = 65535 / (22000 × seconds)
```

State machine:
- **Attack** — `envLevel` ramps up at `attackRate` per sample until it hits 65535, then moves to Decay
- **Decay** — `envLevel` ramps down at `decayRate` until it reaches `sustainLevel`, then moves to Sustain
- **Sustain** — `envLevel` holds at `sustainLevel` while the key is held
- **Release** — triggered by NOTE_OFF or HOLD_OFF; `envLevel` ramps down at `releaseRate` until
  it hits zero, at which point the voice deactivates and frees itself

The envelope multiplier applied to each voice's output is `envLevel >> 8` (giving 0–255).

---

## Biquad Filter

Each instrument uses a 2nd-order Butterworth low-pass biquad filter applied to the final mixed
signal in `tick()`. This replaced the original single-pole IIR.

A biquad gives −40 dB/octave roll-off (vs −20 dB/octave for a single-pole filter) and exact
frequency control via coefficients computed offline using the Audio EQ Cookbook formula:

```
w0 = 2π × f0 / Fs
α  = sin(w0) / (2Q)    Q = 0.7071 (Butterworth — maximally flat passband)

b0 = b2 = (1 − cos(w0)) / 2
b1 =       1 − cos(w0)
a0 =       1 + α
a1 =      −2 × cos(w0)
a2 =       1 − α

Normalise all by a0, then scale to Q1.14 (multiply by 16384)
```

Coefficients stored as `int16_t` in the preset. Filter runs entirely in `int32_t` arithmetic —
no floating point at runtime. Biquad state (`bqX1_, bqX2_, bqY1_, bqY2_`) lives in the `Synth`
class and is shared across all voices on the mix output.

---

## Loudness Normalisation

Different waveforms have different RMS energy at the same peak amplitude. Without correction,
square wave (MIDI) sounds ~1.7× louder than saw or sine at the same volume knob setting.

Each preset includes a `gain` value applied as `(noteVout * gain) >> 7`, where 128 = unity:

| Instrument | Waveform | Gain |
|---|---|---|
| MIDI | Square | 110 |
| Piano | Saw | 185 |
| Violin | SuperSaw | 252 |
| Flute | Sine | 252 |

---

## Instruments

### Piano — Saw wave

A piano string is struck and immediately begins decaying. The saw wave is rich in both odd and
even harmonics, matching the complex spectrum of a struck string. The fast attack mirrors the
hammer strike. The long decay drops to a moderate sustain, and the biquad at 3500 Hz removes
the harsh upper partials of the raw saw to give a warmer, more acoustic character.

| Parameter | Value |
|---|---|
| Oscillator | Saw |
| Attack | ~3ms |
| Decay | ~800ms |
| Sustain | 40% |
| Release | ~300ms |
| Filter | LP 3500 Hz |

---

### MIDI — Square wave

Raw digital synthesis with no envelope shaping and no filtering. Instant on/off, full volume,
square wave. Intentionally bypasses all the acoustic modelling added to the other instruments.
Useful as a reference tone or for a classic 8-bit / chiptune character.

| Parameter | Value |
|---|---|
| Oscillator | Square |
| Attack | Instant |
| Decay | None |
| Sustain | 100% |
| Release | Instant |
| Filter | None (identity) |

---

### Violin — SuperSaw wave

A bowed violin builds slowly as bow pressure establishes the vibration (slow 300ms attack) and
sustains at high volume while the bow moves. The SuperSaw waveform (two slightly detuned saws
averaged together) creates the slight chorusing and beating effect of the instrument's rich tone.
The biquad at 2000 Hz gives the warm, dark body of a bowed string.

| Parameter | Value |
|---|---|
| Oscillator | SuperSaw |
| Attack | ~300ms |
| Decay | ~200ms |
| Sustain | 85% |
| Release | ~300ms |
| Filter | LP 2000 Hz |

---

### Flute — Sine wave

A flute produces a nearly pure tone dominated by the fundamental with very few overtones. The
sine wave is the mathematically pure waveform with no harmonics, making it the closest match.
The gentle 50ms attack reflects the time for breath to establish the air column. The biquad at
4500 Hz barely alters the tone — it is there mainly to smooth any DAC quantisation artefacts
at high frequencies.

| Parameter | Value |
|---|---|
| Oscillator | Sine |
| Attack | ~50ms |
| Decay | ~100ms |
| Sustain | 70% |
| Release | ~200ms |
| Filter | LP 4500 Hz |
