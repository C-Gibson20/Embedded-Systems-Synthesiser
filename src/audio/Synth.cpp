#include "Synth.h"
#include "sine_lut.h"
#include "pins.h"
#include <STM32FreeRTOS.h>

Synth synth;

static HardwareTimer sampleTimer_(TIM1);

// ================================================= //
// =============== Waveform functions ============== //
// ================================================= //

static inline uint32_t funcSquare(uint8_t i)  { return (i < 128) ? 255 : 0; }
static inline uint32_t funcSaw(uint8_t i)      { return i; }
static inline uint32_t funcTri(uint8_t i)      { return (i < 128) ? (i << 1) : (511 - (i << 1)); }
static inline uint32_t funcSine(uint8_t i)     { return sineTable[i]; }

static inline uint32_t funcSuperSaw(uint8_t i) {
    uint8_t saw2 = (i + (i >> 2)) & 0xFF;
    return (i + saw2) >> 1;
}

static inline uint32_t funcSineFold(uint8_t i) {
    int16_t val = (sineTable[i] - 128) * 2;
    if (val > 127)  val = 255 - val;
    if (val < -128) val = -255 - val;
    return val + 128;
}

// ================================================= //
// ================ Synth private ================== //
// ================================================= //

uint32_t Synth::computeStep(uint8_t key, uint8_t octave) {
    uint32_t step = STEP_SIZES[key];
    int8_t shift = octave - 4;
    if (shift > 0) step <<= shift;
    else if (shift < 0) step >>= abs(shift);
    return step;
}

int Synth::allocateSound() {
    if (freeTop_ == 0) return -1;
    freeTop_--;
    return freeSounds_[freeTop_];
}

void Synth::freeSound(int idx) {
    if (freeTop_ < MAX_VOICES) {
        freeSounds_[freeTop_] = idx;
        freeTop_++;
    }
}

void Synth::pushAudioCommand(const AudioCommand& cmd) {
    taskENTER_CRITICAL();
    uint8_t nextWriteIdx = (audioCommandWriteIdx_ + 1) % AUDIO_COMMAND_QUEUE_LENGTH;
    if (nextWriteIdx != audioCommandReadIdx_) {
        audioCommandQueue_[audioCommandWriteIdx_] = cmd;
        __DMB();
        audioCommandWriteIdx_ = nextWriteIdx;
    }
    taskEXIT_CRITICAL();
}

// ================================================= //
// ================= Synth public ================== //
// ================================================= //

void Synth::begin() {
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        freeSounds_[i] = i;
        sounds[i].active   = false;
        sounds[i].waveform = SQUARE;
        sounds[i].volume   = 0;
        sounds[i].phase    = 0;
    }
    freeTop_ = MAX_VOICES;

    analogWrite(OUTR_PIN, 128);
    sampleBufferSemaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(sampleBufferSemaphore);       
    memset(sampleBuffer0, 128, BUFFER_SIZE);       
    memset(sampleBuffer1, 128, BUFFER_SIZE);
    
    sampleTimer_.setOverflow(22000, HERTZ_FORMAT);
    #ifndef DISABLE_ISRS
        sampleTimer_.attachInterrupt(sampleISR);
    #endif
    sampleTimer_.resume();
}

void Synth::processCommands() {
    uint8_t writeIdx = audioCommandWriteIdx_;
    __DMB();
    while (audioCommandReadIdx_ != writeIdx) {
        AudioCommand cmd = audioCommandQueue_[audioCommandReadIdx_];
        audioCommandReadIdx_ = (audioCommandReadIdx_ + 1) % AUDIO_COMMAND_QUEUE_LENGTH;

        switch (cmd.type) {
            case NOTE_ON: {
                int idx = allocateSound();
                if (idx >= 0) {
                    sounds[idx].step          = cmd.step;
                    sounds[idx].pitch         = cmd.pitch;
                    sounds[idx].effectiveStep = cmd.effectiveStep;
                    sounds[idx].phase         = 0;
                    sounds[idx].volume        = cmd.volume;
                    sounds[idx].waveform      = cmd.waveform;
                    sounds[idx].key           = cmd.key;
                    sounds[idx].held          = false;
                    sounds[idx].remote        = cmd.remote;
                    sounds[idx].active        = true;
                }
                break;
            }
            case NOTE_OFF: {
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (sounds[i].active && sounds[i].key == cmd.key &&
                        sounds[i].remote == cmd.remote && !sounds[i].held) {
                        sounds[i].active = false;
                        freeSound(i);
                    }
                }
                break;
            }
            case HOLD_ON: {
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (sounds[i].active && !sounds[i].remote) sounds[i].held = true;
                }
                break;
            }
            case HOLD_OFF: {
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (sounds[i].held && sounds[i].active) {
                        sounds[i].active = false;
                        freeSound(i);
                    }
                }
                break;
            }
            case ROLE_CHANGE: {
                if (cmd.newRole != RECEIVER) {
                    for (int i = 0; i < MAX_VOICES; i++) {
                        if (sounds[i].remote && sounds[i].active) {
                            sounds[i].active = false;
                            freeSound(i);
                        }
                    }
                }
                if (cmd.newRole == SENDER) {
                    for (int i = 0; i < MAX_VOICES; i++) {
                        if (!sounds[i].remote && sounds[i].active) {
                            sounds[i].active = false;
                            freeSound(i);
                        }
                    }
                }
                break;
            }
        }
    }
}

void Synth::fillBuffer() {
    processCommands();
    
    uint8_t* buf = writeBuffer1 ? sampleBuffer1 : sampleBuffer0;
    
    for (uint32_t i = 0; i < BUFFER_SIZE; i++) {
        buf[i] = tick();
    }
}

uint32_t Synth::tick() {
    int32_t mixedVout   = 0;
    uint8_t activeNotes = 0;

    for (int i = 0; i < MAX_VOICES; i++) {
        volatile Sound& s = sounds[i];
        if (!s.active) continue;

        s.phase += s.effectiveStep;
        uint8_t index = s.phase >> 24;
        uint32_t uncenteredValue;
        switch (s.waveform) {
            case SQUARE:   uncenteredValue = funcSquare(index);   break;
            case SAW:      uncenteredValue = funcSaw(index);      break;
            case TRIANGLE: uncenteredValue = funcTri(index);      break;
            case SINE:     uncenteredValue = funcSine(index);     break;
            case SUPERSAW: uncenteredValue = funcSuperSaw(index); break;
            case SINEFOLD: uncenteredValue = funcSineFold(index); break;
            default:       uncenteredValue = 128;                  break;
        }

        int32_t noteVout = (int32_t)(uncenteredValue) - 128;
        noteVout >>= (8 - s.volume);
        mixedVout += noteVout;
        activeNotes++;
    }

    if (activeNotes == 0) return 128;

    static const uint16_t invGain[] = {
        0, 256/1, 256/2, 256/3, 256/4, 256/5, 256/6, 256/7, 256/8,
        256/9, 256/10, 256/11, 256/12, 256/13, 256/14, 256/15, 256/16
    };
    if (activeNotes > 1) {
        mixedVout = (mixedVout * invGain[activeNotes]) >> 8;
    }
    return (uint32_t)(mixedVout + 128);
}

void Synth::pushNoteOn(uint8_t key, uint8_t volume, int32_t pitch, int waveform, bool remote, uint8_t octave) {
    uint32_t baseStep = computeStep(key, octave);
    int32_t  offset   = (baseStep * (pitch >> 2)) >> 6;

    AudioCommand cmd;
    cmd.type          = NOTE_ON;
    cmd.key           = key;
    cmd.step          = baseStep;
    cmd.volume        = volume;
    cmd.pitch         = pitch;
    cmd.waveform      = (SynthWaveform)waveform;
    cmd.remote        = remote;
    cmd.effectiveStep = baseStep + offset;
    pushAudioCommand(cmd);
}

void Synth::pushNoteOff(uint8_t key, bool remote) {
    AudioCommand cmd;
    cmd.type   = NOTE_OFF;
    cmd.key    = key;
    cmd.remote = remote;
    pushAudioCommand(cmd);
}

void Synth::pushHold(AudioCommandType type) {
    AudioCommand cmd;
    cmd.type = type;
    pushAudioCommand(cmd);
}

void Synth::pushRoleChange(SynthRole newRole) {
    AudioCommand cmd;
    cmd.type    = ROLE_CHANGE;
    cmd.newRole = newRole;
    pushAudioCommand(cmd);
}

void Synth::updateGlobalParams(uint32_t& lastPitch, uint8_t& lastVolume, uint8_t& lastWaveform, uint8_t& lastOctave,
                                uint32_t pitch, uint8_t volume, uint8_t waveform, uint8_t octave) {
    lastPitch    = pitch;
    lastVolume   = volume;
    lastWaveform = waveform;
    lastOctave   = octave;

    globalParams_.pitch    = pitch;
    globalParams_.volume   = volume;
    globalParams_.waveform = (SynthWaveform)waveform;
    globalParams_.octave   = octave;

    __DMB();
    globalParams_.hasChanged = true;
}

void Synth::applyGlobalParamUpdates() {
    uint8_t       currentVol    = globalParams_.volume;
    SynthWaveform currentWave   = globalParams_.waveform;
    uint8_t       currentOctave = globalParams_.octave;
    int32_t       currentPitch  = globalParams_.pitch;

    for (int i = 0; i < MAX_VOICES; i++) {
        if (sounds[i].active && !sounds[i].held && !sounds[i].remote) {
            sounds[i].volume   = currentVol;
            sounds[i].waveform = currentWave;
            sounds[i].pitch    = currentPitch;

            uint32_t baseStep = computeStep(sounds[i].key, currentOctave);
            int32_t  offset   = (baseStep * (currentPitch >> 2)) >> 6;
            sounds[i].effectiveStep = baseStep + offset;
        }
    }
}

void Synth::resetCommandQueue() {
    audioCommandWriteIdx_ = 0;
    audioCommandReadIdx_  = 0;
}

// ================================================= //
// ==================== ISR ======================== //
// ================================================= //

void sampleISR() {
    if (synth.readCtr == Synth::BUFFER_SIZE) {
        synth.readCtr = 0;
        synth.writeBuffer1 = !synth.writeBuffer1;
        xSemaphoreGiveFromISR(synth.sampleBufferSemaphore, NULL);
    }

    if (synth.writeBuffer1)
            DAC1->DHR8R1 = synth.sampleBuffer0[synth.readCtr++];
        else
            DAC1->DHR8R1 = synth.sampleBuffer1[synth.readCtr++];
}
