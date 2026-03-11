#pragma once                                                                                                                            
#include <bitset>   
#include <array>                                                           
#include <STM32FreeRTOS.h>

enum SynthRole { SENDER, RECEIVER, SINGLE };
enum SynthWaveform {SQUARE, SAW, TRIANGLE, SINE, SUPERSAW, SINEFOLD};
enum OctaveControlMode {OCTAVE_LOCAL, OCTAVE_OFFSET};
enum JoyState { JOY_NEUTRAL, JOY_UP, JOY_DOWN };

struct DisplayState {
    uint8_t waveform;
    uint8_t volume;
    int8_t pitch;
    int8_t octave;
    SynthRole role;
    OctaveControlMode octaveMode;
    uint16_t activeNotes;
};

struct Sound {
    uint32_t step;
    volatile uint32_t effectiveStep; // Step after pitch modulation
    uint32_t phase;
    uint8_t volume;
    int32_t pitch;
    SynthWaveform waveform;
    uint8_t key;
    bool active;
    bool held;
    bool remote; 
};

struct SysState{
    SynthRole role = SINGLE;
    std::bitset<32> inputs;
    bool hold = false;
    std::array<uint8_t, 8> RX_Message = {0};
    std::array<uint8_t, 8> TX_Message = {0};
    OctaveControlMode octaveMode = OCTAVE_LOCAL;
    DisplayState displayState;
    int8_t keyToSound[12] = {-1}; // Maps each key to its active sound index, or -1 if no active sound
    SemaphoreHandle_t mutex;
    volatile JoyState joyState = JOY_NEUTRAL;
};

extern SysState sysState;