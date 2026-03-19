#pragma once
#include <Arduino.h>
#include "profiling.h"
#include "SysState.h"
#include "constants.h"

extern DAC_HandleTypeDef hdac1;
extern TIM_HandleTypeDef htim;
extern DMA_HandleTypeDef hdma_dac_ch1;

enum AudioCommandType { NOTE_ON, NOTE_OFF, HOLD_ON, HOLD_OFF, ROLE_CHANGE };

struct AudioCommand {
    AudioCommandType type;
    SynthRole        newRole;
    uint8_t          key;
    uint32_t         step;
    uint32_t         effectiveStep;
    uint8_t          volume;
    int32_t          pitch;
    Instrument       instrument;
    bool             remote;
};

struct GlobalParameters {
    volatile uint8_t     volume;
    volatile Instrument  instrument;
    volatile uint8_t     octave;
    volatile int32_t     pitch;
    volatile bool        hasChanged;
};

class Synth {
public:
    static constexpr uint32_t BUFFER_SIZE = 64*2;

    uint8_t sampleBuffer[BUFFER_SIZE];
    volatile bool writeBuffer1 = false;
    SemaphoreHandle_t sampleBufferSemaphore;

    Sound sounds[MAX_VOICES];
    uint16_t activeNotesBitmask = 0; // written by sampleGenTask, read by displayUpdateTask — atomic on ARM Cortex-M4

    void begin();
    void processCommands();
    void fillBuffer();
    uint32_t tick();

    void pushNoteOn(uint8_t key, uint8_t volume, int32_t pitch, int instrument, bool remote, uint8_t octave);
    void pushNoteOff(uint8_t key, bool remote);
    void pushHold(AudioCommandType type);
    void pushRoleChange(SynthRole newRole);
    void updateGlobalParams(uint32_t& lastPitch, uint8_t& lastVolume, uint8_t& lastWaveform, uint8_t& lastOctave,
                            uint32_t pitch, uint8_t volume, uint8_t instrument, uint8_t octave);
    void applyGlobalParamUpdates();
    void resetCommandQueue();

private:
    volatile uint8_t freeSounds_[MAX_VOICES];
    volatile uint8_t freeTop_ = 0;
    GlobalParameters globalParams_ = {};
    int32_t bqX1_ = 0, bqX2_ = 0; // biquad input state
    int32_t bqY1_ = 0, bqY2_ = 0; // biquad output state
    AudioCommand     audioCommandQueue_[AUDIO_COMMAND_QUEUE_LENGTH];
    volatile uint8_t audioCommandWriteIdx_ = 0;
    volatile uint8_t audioCommandReadIdx_  = 0;

    int      allocateSound();
    void     freeSound(int idx);
    uint32_t computeStep(uint8_t key, uint8_t octave);
    void     pushAudioCommand(const AudioCommand& cmd);
};

extern Synth synth;
