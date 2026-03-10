#pragma once
#include <Arduino.h>
#include "SysState.h"
#include "constants.h"

enum AudioCommandType { NOTE_ON, NOTE_OFF, HOLD_ON, HOLD_OFF, ROLE_CHANGE };

struct AudioCommand {
    AudioCommandType type;
    SynthRole        newRole;
    uint8_t          key;
    uint32_t         step;
    uint32_t         effectiveStep;
    uint8_t          volume;
    int32_t          pitch;
    SynthWaveform    waveform;
    bool             remote;
    bool             updatePitch;
    bool             updateVolume;
    bool             updateWave;
    bool             updateOctave;
    int8_t           octaveValue;
};

struct GlobalParameters {
    volatile uint8_t       volume;
    volatile SynthWaveform waveform;
    volatile uint8_t       octave;
    volatile int32_t       pitch;
    volatile bool          hasChanged;
};

class Synth {
public:
    static constexpr uint32_t BUFFER_SIZE = 64;

    uint8_t sampleBuffer0[BUFFER_SIZE];
    uint8_t sampleBuffer1[BUFFER_SIZE];
    volatile bool writeBuffer1 = false;
    volatile uint32_t readCtr = 0;
    SemaphoreHandle_t sampleBufferSemaphore;

    volatile Sound sounds[MAX_VOICES];

    void begin();
    void processCommands();
    void fillBuffer();
    uint32_t tick();

    void pushNoteOn(uint8_t key, uint8_t volume, int32_t pitch, int waveform, bool remote, uint8_t octave);
    void pushNoteOff(uint8_t key, bool remote);
    void pushHold(AudioCommandType type);
    void pushRoleChange(SynthRole newRole);
    void updateGlobalParams(uint32_t& lastPitch, uint8_t& lastVolume, uint8_t& lastWaveform, uint8_t& lastOctave,
                            uint32_t pitch, uint8_t volume, uint8_t waveform, uint8_t octave);
    void applyGlobalParamUpdates();
    void resetCommandQueue();

private:
    volatile uint8_t freeSounds_[MAX_VOICES];
    volatile uint8_t freeTop_ = 0;
    GlobalParameters globalParams_ = {};
    AudioCommand     audioCommandQueue_[AUDIO_COMMAND_QUEUE_LENGTH];
    volatile uint8_t audioCommandWriteIdx_ = 0;
    volatile uint8_t audioCommandReadIdx_  = 0;

    int      allocateSound();
    void     freeSound(int idx);
    uint32_t computeStep(uint8_t key, uint8_t octave);
    void     pushAudioCommand(const AudioCommand& cmd);
};

extern Synth synth;
void sampleISR();
