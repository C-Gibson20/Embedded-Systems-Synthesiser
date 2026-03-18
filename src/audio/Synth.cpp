#include "Synth.h"
#include "sine_lut.h"
#include "pins.h"
#include <STM32FreeRTOS.h>

DAC_HandleTypeDef hdac1;
TIM_HandleTypeDef htim;
DMA_HandleTypeDef hdma_dac_ch1;

Synth synth;

static void MX_DAC1_Init(void)
{
    __HAL_RCC_DAC1_CLK_ENABLE();
    DAC_ChannelConfTypeDef sConfig = {0};
    hdac1.Instance = DAC1;

    if (HAL_DAC_Init(&hdac1) != HAL_OK)
    {
        digitalWrite(LED_BUILTIN, HIGH);  // Stuck here
        while(1);
    }

    sConfig.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
    sConfig.DAC_Trigger = DAC_TRIGGER_T6_TRGO;
    sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
    sConfig.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_DISABLE;
    sConfig.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
    if (HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1) != HAL_OK)
    {
        digitalWrite(LED_BUILTIN, HIGH);  // Stuck here
        while(1);
    }

}

extern "C" void DMA1_Channel3_IRQHandler(void) {
    HAL_DMA_IRQHandler(&hdma_dac_ch1);
}

static void MX_DMA_Init(void)
{

    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_dac_ch1.Instance = DMA1_Channel3;
    hdma_dac_ch1.Init = {
        .Request           = DMA_REQUEST_6,
        .Direction         = DMA_MEMORY_TO_PERIPH,
        .PeriphInc         = DMA_PINC_DISABLE,
        .MemInc            = DMA_MINC_ENABLE,
        .PeriphDataAlignment = DMA_PDATAALIGN_BYTE,
        .MemDataAlignment  = DMA_MDATAALIGN_BYTE,
        .Mode              = DMA_CIRCULAR,
        .Priority          = DMA_PRIORITY_LOW
    };

    if (HAL_DMA_Init(&hdma_dac_ch1) != HAL_OK) {
        digitalWrite(LED_BUILTIN, HIGH);  // Stuck here
        while(1);
    }

    __HAL_LINKDMA(&hdac1, DMA_Handle1, hdma_dac_ch1);

    HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);

}

static void MX_TIM6_Init(void)
{
    __HAL_RCC_TIM6_CLK_ENABLE();

    htim.Instance = TIM6;
    htim.Init = {
        .Prescaler = 0,
        .CounterMode = TIM_COUNTERMODE_UP,
        .Period = 3635,
        .AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE
    };

    if (HAL_TIM_Base_Init(&htim) != HAL_OK) {
        digitalWrite(LED_BUILTIN, HIGH);  // Stuck here
        while(1);
    } 
    TIM_MasterConfigTypeDef masterConfig = {
        .MasterOutputTrigger = TIM_TRGO_UPDATE,
        .MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE
    };

    HAL_TIMEx_MasterConfigSynchronization(&htim, &masterConfig);
    HAL_TIM_Base_Start(&htim);
}

extern "C" void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef* hdac) {
    synth.writeBuffer1 = false;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(synth.sampleBufferSemaphore, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

extern "C" void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef* hdac) {
    synth.writeBuffer1 = true;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(synth.sampleBufferSemaphore, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// static HardwareTimer sampleTimer_(TIM1);

// ================================================= //
// ============== Instrument presets ============== //
// ================================================= //
// ADSR rates are per-sample on a 0-65535 envLevel (22 kHz).
// rate = 65535 / (22000 * seconds).  sustainLevel = percent * 655.
struct InstrumentPreset {
    SynthWaveform waveform;
    uint16_t attackRate;
    uint16_t decayRate;
    uint16_t sustainLevel;
    uint16_t releaseRate;
    uint8_t  gain;    // loudness normalisation: (noteVout * gain) >> 7, 128 = unity
    // Biquad low-pass coefficients in Q1.14 (scale = 16384 = 1.0).
    // Computed offline as 2nd-order Butterworth LP at the cutoff below.
    // Formula: w0=2π*f0/Fs, α=sin(w0)/(2Q), b0=b2=(1-cos(w0))/2, b1=1-cos(w0),
    //          a0=1+α, a1=-2cos(w0), a2=1-α  →  normalise all by a0.
    int16_t b0, b1, b2; // feedforward
    int16_t a1, a2;     // feedback (sign convention: y -= a1*y1 + a2*y2)
};

// ADSR rate = 65535 / (22000 * seconds).  sustainLevel = (percent / 100.0) * 65535.
//                          wave       atk    dec   sus    rel   gain  b0     b1     b2     a1       a2
static const InstrumentPreset PRESETS[] = {
    { SAW,      993,   4,    26214, 10,   170,  2360,  4720,  2360,  -11108,  4160 }, // PIANO   — LP 3500 Hz
    { SQUARE,   65535, 65535,65535, 65535,80,   16384, 0,     0,     0,       0    }, // MIDI    — identity (no filter)
    { SUPERSAW, 10,    15,   55705, 10,   245,  939,   1879,  939,   -19951,  7327 }, // VIOLIN  — LP 2000 Hz
    { SINE,     60,    30,   45875, 15,   255,  3515,  7031,  3515,  -5451,   3131 }, // FLUTE   — LP 4500 Hz
};

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
        freeSounds_[i]         = i;
        sounds[i].active       = false;
        sounds[i].instrument   = PIANO;
        sounds[i].waveform     = PRESETS[PIANO].waveform;
        sounds[i].volume       = 0;
        sounds[i].phase        = 0;
        sounds[i].envPhase     = ENV_ATTACK;
        sounds[i].envLevel     = 0;
        sounds[i].attackRate   = PRESETS[PIANO].attackRate;
        sounds[i].decayRate    = PRESETS[PIANO].decayRate;
        sounds[i].sustainLevel = PRESETS[PIANO].sustainLevel;
        sounds[i].releaseRate  = PRESETS[PIANO].releaseRate;
        sounds[i].gain         = PRESETS[PIANO].gain;
    }
    freeTop_ = MAX_VOICES;
    bqX1_ = bqX2_ = bqY1_ = bqY2_ = 0;
    
    // analogWrite(OUTR_PIN, 128);
    sampleBufferSemaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(sampleBufferSemaphore);       
    memset(sampleBuffer, 128, BUFFER_SIZE);       
    
    MX_DMA_Init();   
    MX_DAC1_Init();  
    MX_TIM6_Init();
    // sampleTimer_.setOverflow(22000, HERTZ_FORMAT);
    // #ifndef DISABLE_ISRS
    //     sampleTimer_.attachInterrupt(sampleISR);
    // #endif
    // sampleTimer_.resume();
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
                    const InstrumentPreset& p = PRESETS[cmd.instrument];
                    sounds[idx].step          = cmd.step;
                    sounds[idx].pitch         = cmd.pitch;
                    sounds[idx].effectiveStep = cmd.effectiveStep;
                    sounds[idx].phase         = 0;
                    sounds[idx].volume        = cmd.volume;
                    sounds[idx].instrument    = cmd.instrument;
                    sounds[idx].waveform      = p.waveform;
                    sounds[idx].attackRate    = p.attackRate;
                    sounds[idx].decayRate     = p.decayRate;
                    sounds[idx].sustainLevel  = p.sustainLevel;
                    sounds[idx].releaseRate   = p.releaseRate;
                    sounds[idx].gain          = p.gain;
                    sounds[idx].key           = cmd.key;
                    sounds[idx].held          = false;
                    sounds[idx].remote        = cmd.remote;
                    sounds[idx].envPhase      = ENV_ATTACK;
                    sounds[idx].envLevel      = 0;
                    sounds[idx].active        = true;
                }
                break;
            }
            case NOTE_OFF: {
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (sounds[i].active && sounds[i].key == cmd.key &&
                        sounds[i].remote == cmd.remote && !sounds[i].held) {
                        sounds[i].envPhase = ENV_RELEASE;
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
                        sounds[i].held     = false;
                        sounds[i].envPhase = ENV_RELEASE;
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
    
    uint32_t start = writeBuffer1 ? BUFFER_SIZE/2 : 0;
    for (uint32_t i = start; i < (start+BUFFER_SIZE/2); i++) {
        sampleBuffer[i] = tick();
    }
}

uint32_t Synth::tick() {
    int32_t mixedVout   = 0;
    uint8_t activeNotes = 0;

    for (int i = 0; i < MAX_VOICES; i++) {
        volatile Sound& s = sounds[i];
        if (!s.active) continue;

        // ADSR envelope state machine
        switch (s.envPhase) {
            case ENV_ATTACK:
                if (s.envLevel + s.attackRate >= 65535) {
                    s.envLevel = 65535;
                    s.envPhase = ENV_DECAY;
                } else {
                    s.envLevel += s.attackRate;
                }
                break;
            case ENV_DECAY:
                if (s.envLevel <= s.sustainLevel + s.decayRate) {
                    s.envLevel = s.sustainLevel;
                    s.envPhase = ENV_SUSTAIN;
                } else {
                    s.envLevel -= s.decayRate;
                }
                break;
            case ENV_SUSTAIN:
                break;
            case ENV_RELEASE:
                if (s.envLevel <= s.releaseRate) {
                    s.active = false;
                    freeSound(i);
                    continue;
                }
                s.envLevel -= s.releaseRate;
                break;
        }

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
        noteVout = (noteVout * s.gain) >> 7;                     // loudness normalisation
        noteVout = (noteVout * (int32_t)(s.envLevel >> 8)) >> 8; // apply envelope
        mixedVout += noteVout;
        activeNotes++;
    }

    static const uint16_t invGain[] = {
        0, 256/1, 256/2, 256/3, 256/4, 256/5, 256/6, 256/7, 256/8,
        256/9, 256/10, 256/11, 256/12, 256/13, 256/14, 256/15, 256/16
    };
    if (activeNotes > 1) {
        mixedVout = (mixedVout * invGain[activeNotes]) >> 8;
    }

    // Biquad low-pass filter — 2nd order Butterworth, coefficients in Q1.14
    const InstrumentPreset& p = PRESETS[globalParams_.instrument];
    int32_t y = ((int32_t)p.b0 * mixedVout
               + (int32_t)p.b1 * bqX1_
               + (int32_t)p.b2 * bqX2_
               - (int32_t)p.a1 * bqY1_
               - (int32_t)p.a2 * bqY2_) >> 14;
    bqX2_ = bqX1_; bqX1_ = mixedVout;
    bqY2_ = bqY1_; bqY1_ = y;
    int32_t out = y + 128;
    if (out > 255) out = 255;
    if (out < 0)   out = 0;
    return (uint32_t)out;
}

void Synth::pushNoteOn(uint8_t key, uint8_t volume, int32_t pitch, int instrument, bool remote, uint8_t octave) {
    uint32_t baseStep = computeStep(key, octave);
    int32_t  offset   = (baseStep * (pitch >> 2)) >> 6;

    AudioCommand cmd;
    cmd.type          = NOTE_ON;
    cmd.key           = key;
    cmd.step          = baseStep;
    cmd.volume        = volume;
    cmd.pitch         = pitch;
    cmd.instrument    = (Instrument)instrument;
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
                                uint32_t pitch, uint8_t volume, uint8_t instrument, uint8_t octave) {
    lastPitch    = pitch;
    lastVolume   = volume;
    lastWaveform = instrument;
    lastOctave   = octave;

    globalParams_.pitch      = pitch;
    globalParams_.volume     = volume;
    globalParams_.instrument = (Instrument)instrument;
    globalParams_.octave     = octave;

    __DMB();
    globalParams_.hasChanged = true;
}

void Synth::applyGlobalParamUpdates() {
    uint8_t    currentVol    = globalParams_.volume;
    Instrument currentInst   = globalParams_.instrument;
    uint8_t    currentOctave = globalParams_.octave;
    int32_t    currentPitch  = globalParams_.pitch;
    const InstrumentPreset& p = PRESETS[currentInst];

    for (int i = 0; i < MAX_VOICES; i++) {
        if (sounds[i].active && !sounds[i].held && !sounds[i].remote) {
            sounds[i].volume       = currentVol;
            sounds[i].instrument   = currentInst;
            sounds[i].waveform     = p.waveform;
            sounds[i].attackRate   = p.attackRate;
            sounds[i].decayRate    = p.decayRate;
            sounds[i].sustainLevel = p.sustainLevel;
            sounds[i].releaseRate  = p.releaseRate;
            sounds[i].gain         = p.gain;
            sounds[i].pitch        = currentPitch;

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