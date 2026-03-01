#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <STM32FreeRTOS.h>
#include <Wire.h>
#include <ES_CAN.h>
#include <bits/stdc++.h>
#include "Knob.h"
#include "sine_lut.h"

// --- PROFILING SYSTEM ---
// #define PROFILING_MODE  
// #define V1

#define V2
#ifdef PROFILING_MODE
    #define DISABLE_THREADS
    #define DISABLE_ISRS
  
    // #define PROFILE_SCANKEYS
    // #define PROFILE_DISPLAY
    // #define PROFILE_DECODE
    // #define PROFILE_KNOB
    // #define PROFILE_CAN_TX

    // #define PROFILE_SAMPLE_ISR
    // #define PROFILE_CAN_RX_ISR
    // #define PROFILE_CAN_TX_ISR
    // #define PROFILE_KNOB_ISR
#endif
// --------------------------- 

// ================================================= //
// --- DIRECT PORT MANIPULATION MACROS (STM32L432KC) //
// ================================================= //
#if defined(ARDUINO_ARCH_STM32)
    // A5 = PA6 (REN_PIN)  <-- FIXED!
    #define REN_HIGH()  (GPIOA->BSRR = (1 << 6))
    #define REN_LOW()   (GPIOA->BSRR = (1 << (6 + 16)))

    // D3 = PB0 (RA0_PIN)
    #define RA0_HIGH()  (GPIOB->BSRR = (1 << 0))
    #define RA0_LOW()   (GPIOB->BSRR = (1 << (0 + 16)))

    // D6 = PB1 (RA1_PIN)
    #define RA1_HIGH()  (GPIOB->BSRR = (1 << 1))
    #define RA1_LOW()   (GPIOB->BSRR = (1 << (1 + 16)))

    // D12 = PB4 (RA2_PIN)
    #define RA2_HIGH()  (GPIOB->BSRR = (1 << 4))
    #define RA2_LOW()   (GPIOB->BSRR = (1 << (4 + 16)))

    // D11 = PB5 (OUT_PIN)
    #define OUT_HIGH()  (GPIOB->BSRR = (1 << 5))
    #define OUT_LOW()   (GPIOB->BSRR = (1 << (5 + 16)))

    // --- INPUT MASKS (All mapped to Port A) ---
    // A2 = PA3 (C0_PIN)   <-- FIXED!
    #define C0_MASK (1 << 3)
    // D9 = PA8 (C1_PIN)
    #define C1_MASK (1 << 8)
    // A6 = PA7 (C2_PIN)   <-- FIXED!
    #define C2_MASK (1 << 7)
    // D1 = PA9 (C3_PIN)
    #define C3_MASK (1 << 9)
#else
    // Fallback if compiled for a different board
    #define REN_HIGH()  (digitalWrite(REN_PIN, HIGH))
    #define REN_LOW()   (digitalWrite(REN_PIN, LOW))
    #define RA0_HIGH()  (digitalWrite(RA0_PIN, HIGH))
    #define RA0_LOW()   (digitalWrite(RA0_PIN, LOW))
    #define RA1_HIGH()  (digitalWrite(RA1_PIN, HIGH))
    #define RA1_LOW()   (digitalWrite(RA1_PIN, LOW))
    #define RA2_HIGH()  (digitalWrite(RA2_PIN, HIGH))
    #define RA2_LOW()   (digitalWrite(RA2_PIN, LOW))
    #define OUT_HIGH()  (digitalWrite(OUT_PIN, HIGH))
    #define OUT_LOW()   (digitalWrite(OUT_PIN, LOW))
#endif
// -----------------------------------------------------

//Constants
const uint32_t displayInterval = 100; 
const uint32_t scanInterval = 20;
  
//PCAL6408A Registers
const uint8_t EXPANDER_ADDR = 0x21;
const uint8_t REG_INPUT = 0x00;
const uint8_t REG_PULL_EN = 0x43;
const uint8_t REG_PULL_SEL = 0x44; 
const uint8_t REG_LAT_EN = 0x42;   
const uint8_t REG_INT_MASK = 0x45; 

//Music Data
const int octave = 4;
const double fs = 22000.0;
const double pow2_32 = 4294967296.0;

const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* waveNames[] = {"SQ","SW","TR","SI","SS","SF"};
constexpr float f_notes[] = {
    261.63f, 277.18f, 293.66f, 311.13f, // C, C#, D, D#
    329.63f, 349.23f, 369.99f, 392.00f, // E, F, F#, G
    415.30f, 440.00f, 466.16f, 493.88f  // G#, A, A#, B
};

constexpr uint32_t stepSizes[] = {
    (uint32_t)(f_notes[0] * pow2_32 / fs),
    (uint32_t)(f_notes[1] * pow2_32 / fs),
    (uint32_t)(f_notes[2] * pow2_32 / fs),
    (uint32_t)(f_notes[3] * pow2_32 / fs),
    (uint32_t)(f_notes[4] * pow2_32 / fs),
    (uint32_t)(f_notes[5] * pow2_32 / fs),
    (uint32_t)(f_notes[6] * pow2_32 / fs),
    (uint32_t)(f_notes[7] * pow2_32 / fs),
    (uint32_t)(f_notes[8] * pow2_32 / fs),
    (uint32_t)(f_notes[9] * pow2_32 / fs),
    (uint32_t)(f_notes[10] * pow2_32 / fs),
    (uint32_t)(f_notes[11] * pow2_32 / fs)
};

//Shared state
enum SynthRole { SENDER, RECEIVER, SINGLE };
enum SynthWaveform {SQUARE, SAW, TRIANGLE, SINE, SUPERSAW, SINEFOLD};
enum OctaveControlMode {OCTAVE_LOCAL, OCTAVE_OFFSET};

struct DisplayState {
    uint8_t waveform;
    uint8_t volume;
    int8_t pitch;
    int8_t octave;
    SynthRole role;
    OctaveControlMode octaveMode;
    uint16_t activeNotes;
};

struct {
    SynthRole role = SINGLE;
    std::bitset<32> inputs;
    bool hold = false;
    std::array<uint8_t, 8> RX_Message = {0};
    std::array<uint8_t, 8> TX_Message = {0};
    OctaveControlMode octaveMode = OCTAVE_LOCAL;
    DisplayState displayState;
    SemaphoreHandle_t mutex;
} sysState;

// SemaphoreHandle_t knobSemaphore;
SemaphoreHandle_t CAN_TX_Semaphore;

Knob knobs[5] = {
    Knob(0, -128, 127),
    Knob(0, 0, 5),
    Knob(4, 0, 8), // Local octave control
    Knob(2, 0, 8),
    Knob(0, -8, 8) // Octave offset for RECEIVER role when in OCTAVE_OFFSET mode  
};
const uint8_t volumeIdx = 3;
const uint8_t octaveIdx = 2;
const uint8_t octaveOffsetIdx = 4;
const uint8_t waveIdx = 1;
const uint8_t pitchIdx = 0;

//CAN Bus Communication
QueueHandle_t msgInQ;
QueueHandle_t msgOutQ;

//Sound Handling
const int MAX_SOUNDS = 16;
enum AudioCommandType {NOTE_ON, NOTE_OFF, HOLD_ON, HOLD_OFF, ROLE_CHANGE}; //, SOUND_UPDATE};

struct Sound {
    uint32_t step;
    uint32_t effectiveStep; // Step after pitch modulation
    uint32_t phase;
    uint8_t volume;
    int32_t pitch;
    SynthWaveform waveform;
    uint8_t key;
    bool active;
    bool held;
    bool remote; // Indicates if the sound was triggered by a remote message
};

struct GlobalParameters {
    volatile uint8_t volume;
    volatile SynthWaveform waveform;
    volatile uint8_t octave;
    volatile int32_t pitch;
};

struct AudioCommand {
    AudioCommandType type;
    SynthRole newRole; // Used only for ROLE_CHANGE commands
    uint8_t key;
    uint32_t step;
    uint8_t volume;
    int32_t pitch;
    SynthWaveform waveform;
    bool remote;
    bool updatePitch;
    bool updateVolume;
    bool updateWave;
    bool updateOctave;
    int8_t octaveValue;
};

volatile Sound sounds[MAX_SOUNDS];
volatile uint8_t freeSounds[MAX_SOUNDS];    
volatile uint8_t freeTop = 0;
volatile GlobalParameters globalParams;

constexpr int AUDIO_COMMAND_QUEUE_LENGTH = 32;
AudioCommand audioCommandQueue[AUDIO_COMMAND_QUEUE_LENGTH];
volatile uint8_t audioCommandWriteIdx = 0;
volatile uint8_t audioCommandReadIdx = 0;

//Pin definitions
//Row select and enable
const int RA0_PIN = D3;
const int RA1_PIN = D6;
const int RA2_PIN = D12;
const int REN_PIN = A5;

//Matrix input and output
const int C0_PIN = A2;
const int C1_PIN = D9;
const int C2_PIN = A6;
const int C3_PIN = D1;
const int OUT_PIN = D11;

//Audio analogue out
const int OUTL_PIN = A4;
const int OUTR_PIN = A3;

//Joystick analogue in
const int JOYY_PIN = A0;
const int JOYX_PIN = A1;

//Output multiplexer bits
const int KNOB_MODE = 2;
const int DEN_BIT = 3;
const int DRST_BIT = 4;
const int HKOW_BIT = 5;
const int HKOE_BIT = 6;

//Display driver object
U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

//Hardware Timer
HardwareTimer sampleTimer(TIM1);

// ================================================= //
// ================ Hardware Helpers =============== //
// ================================================= //

//Function to set outputs using key matrix
void setOutMuxBit(const uint8_t bitIdx, const bool value) {
    REN_LOW();

    if (bitIdx & 0x01) RA0_HIGH(); else RA0_LOW();
    if (bitIdx & 0x02) RA1_HIGH(); else RA1_LOW();
    if (bitIdx & 0x04) RA2_HIGH(); else RA2_LOW();

    if (value) OUT_HIGH(); else OUT_LOW();

    REN_HIGH();
    delayMicroseconds(1);
    REN_LOW();
}

// Function to read the inputs from the four columns of the switch matrix
std::bitset<4> readCols() {
    std::bitset<4> result;

    uint32_t portAState = GPIOA->IDR; // Read entire Port A state
    result[0] = (portAState & C0_MASK) != 0;
    result[1] = (portAState & C1_MASK) != 0;
    result[2] = (portAState & C2_MASK) != 0;
    result[3] = (portAState & C3_MASK) != 0;

    return result;
}

void setRow(uint8_t rowIdx){
    // Set Row Select Enable low
    REN_LOW();

    // Set Row Select Address low
    if (rowIdx & 0x01) RA0_HIGH(); else RA0_LOW();
    if (rowIdx & 0x02) RA1_HIGH(); else RA1_LOW();
    if (rowIdx & 0x04) RA2_HIGH(); else RA2_LOW();

    // Set Row Select Enable High
    REN_HIGH();
    delayMicroseconds(1);
}

// ================================================= //
// ========= Interrupt Subroutine Helpers ===-====== //
// ================================================= //

uint32_t computeStep(uint8_t key, uint8_t octave) {
    uint32_t step = stepSizes[key];
    int8_t shift = octave - 4;
    if (shift > 0) step <<= shift; 
    else if (shift < 0) step >>= abs(shift);
    return step;
}

int allocateSound() {
    if (freeTop == 0) return -1; // no free voice
    freeTop--;
    return freeSounds[freeTop];
}

void freeSound(int idx) {
    if (freeTop < MAX_SOUNDS) {
        freeSounds[freeTop] = idx;
        freeTop++;
    }
}

void processAudioCommands() {
    uint8_t writeIdx = audioCommandWriteIdx;
    __DMB();
    while (audioCommandReadIdx != writeIdx) {
        AudioCommand cmd = audioCommandQueue[audioCommandReadIdx];
        audioCommandReadIdx = (audioCommandReadIdx + 1) % AUDIO_COMMAND_QUEUE_LENGTH;

        switch (cmd.type) {
            case NOTE_ON: {
                int idx = allocateSound();
                if (idx >= 0) {
                    sounds[idx].step = cmd.step;
                    sounds[idx].pitch = cmd.pitch;

                    int32_t offset = (cmd.step * (cmd.pitch >> 2)) >> 6;
                    sounds[idx].effectiveStep = cmd.step + offset;
                    sounds[idx].phase = 0;
                    sounds[idx].volume = cmd.volume;
                    sounds[idx].waveform = cmd.waveform;
                    sounds[idx].key = cmd.key;
                    sounds[idx].active = true;
                    sounds[idx].held = false;
                    sounds[idx].remote = cmd.remote;
                }
                break;
            }
            case NOTE_OFF: {
                for (int i = 0; i < MAX_SOUNDS; i++) {
                    if (sounds[i].active && sounds[i].key == cmd.key && sounds[i].remote == cmd.remote && !sounds[i].held) {
                        sounds[i].active = false;
                        freeSound(i);
                    }
                }
                break;
            }
            case HOLD_ON: {
                for (int i = 0; i < MAX_SOUNDS; i++) {
                    if (sounds[i].active && !sounds[i].remote) sounds[i].held = true;
                }
                break;
            }
            case HOLD_OFF: {
                for (int i = 0; i < MAX_SOUNDS; i++) {
                    if (sounds[i].held && sounds[i].active) {
                        sounds[i].active = false;
                        freeSound(i);
                    }
                }
                break;
            }
            case ROLE_CHANGE: {
                if (cmd.newRole != RECEIVER) {
                    // Leaving receiver so stop remote voices
                    for (int i = 0; i < MAX_SOUNDS; i++) {
                        if (sounds[i].remote && sounds[i].active) {
                            sounds[i].active = false;
                            freeSound(i);
                        }
                    }
                }

                if (cmd.newRole == SENDER) {
                    // Clear local voices when entering sender
                    for (int i = 0; i < MAX_SOUNDS; i++) {
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

// ================================================= //
// ============= Interrupt Subroutines ============= //
// ================================================= //

void sampleISR() {
    processAudioCommands();

    int32_t mixedVout = 0;
    uint8_t activeNotes = 0;
    
    for (int i = 0; i < MAX_SOUNDS; i++) {

        if(!sounds[i].active) continue;

        if (!sounds[i].held && !sounds[i].remote) {
            sounds[i].volume = globalParams.volume;
            sounds[i].waveform = globalParams.waveform;
            sounds[i].pitch = globalParams.pitch;

            // Recalculate step based on global octave
            uint32_t baseStep = computeStep(sounds[i].key, globalParams.octave);
            int32_t offset = (baseStep * (sounds[i].pitch >> 2)) >> 6;
            sounds[i].effectiveStep = baseStep + offset;
        }

        sounds[i].phase += sounds[i].effectiveStep;
        
        uint8_t index = sounds[i].phase >> 24;
        uint32_t uncenteredValue = 0;

        switch (sounds[i].waveform) {
            case SQUARE:
                uncenteredValue = (index < 128) ? 255 : 0;
                break;
            case SAW:
                uncenteredValue = index;
                break;
            case TRIANGLE:
                uncenteredValue = (index < 128) ? (index << 1) : (511 - (index << 1));
                break;
            case SINE:
                uncenteredValue = sineTable[index];
                break;
            case SUPERSAW: {
                uint8_t saw1 = index;
                uint8_t saw2 = (index + (index >> 2)) & 0xFF; 
                uncenteredValue = (saw1 + saw2) >> 1;
                break;
            }
            case SINEFOLD: {
                int16_t val = (sineTable[index] - 128) * 2; 
                if (val > 127) val = 255 - val;             
                if (val < -128) val = -255 - val;           
                uncenteredValue = val + 128;
                break;
            }
        }

        int32_t noteVout = (int32_t)(uncenteredValue) - 128;
        noteVout >>= (8 - sounds[i].volume); // Apply volume control
        mixedVout += noteVout;
        activeNotes++;
    }

    static const uint16_t invGain[] = {
        0,
        256/1, 256/2, 256/3, 256/4,
        256/5, 256/6, 256/7, 256/8,
        256/9, 256/10, 256/11, 256/12,
        256/13, 256/14, 256/15, 256/16
    };

    if (activeNotes > 0) {
        mixedVout = (mixedVout * invGain[activeNotes]) >> 8;
    }

    analogWrite(OUTR_PIN, mixedVout + 128);
}

void CAN_RX_ISR (void) {
    std::array<uint8_t, 8> RX_Message_ISR;
    uint32_t ID;
    #ifdef PROFILING_MODE
        RX_Message_ISR = {'P', 4, 1, 0, 0, 0, 0, 0}; 
        ID = 0x123;
        xQueueSend(msgInQ, RX_Message_ISR.data(), 0);
    #else
        CAN_RX(ID, RX_Message_ISR.data());
        xQueueSendFromISR(msgInQ, RX_Message_ISR.data(), NULL);
    #endif
}

void CAN_TX_ISR (void) {
	#ifdef PROFILING_MODE
        xSemaphoreGive(CAN_TX_Semaphore);
    #else
        xSemaphoreGiveFromISR(CAN_TX_Semaphore, NULL);
    #endif
}

// ================================================= //
// ================== Task Helpers ================= //
// ================================================= //

void pushAudioCommand(const AudioCommand &audioCmd) {
    // Enter critical section: No other task or ISR can interrupt this block
    taskENTER_CRITICAL();

    uint8_t nextWriteIdx = (audioCommandWriteIdx + 1) % AUDIO_COMMAND_QUEUE_LENGTH;
    
    if (nextWriteIdx != audioCommandReadIdx) {
        audioCommandQueue[audioCommandWriteIdx] = audioCmd;
        __DMB();  // Ensure command is fully written before updating index
        audioCommandWriteIdx = nextWriteIdx;
    }

    // Exit critical section: Normal scheduling resumes
    taskEXIT_CRITICAL();
}

void pushRoleChangeCommand(SynthRole newRole) {
    AudioCommand cmd;
    cmd.type = ROLE_CHANGE;
    cmd.newRole = newRole;
    pushAudioCommand(cmd);
}

void pushHoldCommand(AudioCommandType hold_type) {
    AudioCommand cmd;
    cmd.type = hold_type;
    pushAudioCommand(cmd);
}

void pushNoteOnCommand(uint8_t key, uint8_t volume, int32_t pitch, int waveform, bool remote, uint8_t octave) {
    AudioCommand cmd;
    cmd.type = NOTE_ON;
    cmd.key = key;
    cmd.step = computeStep(key, octave);
    cmd.volume = volume;
    cmd.pitch = pitch;
    cmd.waveform = (SynthWaveform)waveform;
    cmd.remote = remote;
    pushAudioCommand(cmd);
}

void pushNoteOffCommand(uint8_t key, bool remote) {
    AudioCommand cmd;
    cmd.type = NOTE_OFF;
    cmd.key = key;
    cmd.remote = remote;
    pushAudioCommand(cmd);
}

void handleSynthRole(bool westConnected, bool eastConnected, bool pitchPressed) {
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    SynthRole role = sysState.role;
    xSemaphoreGive(sysState.mutex);

    static SynthRole lastRole = SINGLE;
    static bool overwrittenAutoConfig = false;

    // Disconnected defaults to single mode and resets auto-config override
    if (!westConnected && !eastConnected) {
        role = SINGLE;
        overwrittenAutoConfig = false;
    }
    
    // Manual override if connected to at least one other device
    else if (pitchPressed) {
        overwrittenAutoConfig = true;
        role = (role == SENDER) ? RECEIVER : SENDER;
    }

    // If auto-configuration has not been overridden, determine role based on connections
    else if (!overwrittenAutoConfig && !westConnected && eastConnected) role = SENDER;
    else if (!overwrittenAutoConfig && westConnected) role = RECEIVER;

    if (role != lastRole) pushRoleChangeCommand(role);
    lastRole = role;

    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    sysState.role = role;
    xSemaphoreGive(sysState.mutex);
}

void updateRotations(uint8_t rowIdx, std::bitset<4> cols, OctaveControlMode localOctaveMode) {
    #ifdef V1
        if (3 <= rowIdx  && rowIdx < 5) {
            uint8_t knobIndex = (rowIdx == 3) ? 3 : 1;
            uint8_t offset = ((knobIndex == 3) && (localOctaveMode == OCTAVE_LOCAL)) ? -1 : +1; 

            knobs[knobIndex].updateRotation(cols[0],cols[1]);
            knobs[knobIndex + offset].updateRotation(cols[2],cols[3]);
        }
    #elifdef V2
        if (rowIdx == 3) {
            knobs[3].updateRotation(cols[0], cols[1]);
            knobs[0].updateRotation(cols[2], cols[3]);
        }
        if (rowIdx == 4) {
            knobs[(localOctaveMode == OCTAVE_LOCAL) ? octaveIdx : octaveOffsetIdx].updateRotation(cols[0], cols[1]);
            knobs[1].updateRotation(cols[2], cols[3]);
        }
    #endif
}

void handleSwitches(bool volumePressed, bool wavePressed, bool octavePressed) {
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    bool localHold = sysState.hold;
    SynthRole localRole = sysState.role;
    OctaveControlMode localOctaveMode = sysState.octaveMode;
    xSemaphoreGive(sysState.mutex);

    if (volumePressed) {
        localHold = true;
        pushHoldCommand(HOLD_ON);
    }

    if (wavePressed) {
        localHold = false;
        pushHoldCommand(HOLD_OFF);
    }

    if (octavePressed && localRole == RECEIVER) localOctaveMode = (localOctaveMode == OCTAVE_LOCAL) ? OCTAVE_OFFSET : OCTAVE_LOCAL;

    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    sysState.hold = localHold;
    sysState.octaveMode = localOctaveMode;
    xSemaphoreGive(sysState.mutex);
}

void updateSwitchesAndConnections(std::bitset<4> cols, uint8_t rowIdx, bool &westConnected, bool &eastConnected) {
    if (rowIdx == 5) {
        westConnected = (cols[3] == 0);
        #ifdef V1s
            knobs[2].updateSwitch(cols[0]); // C0: Knob 0 S
            knobs[3].updateSwitch(cols[1]); // C1: Knob 3 S
        #elifdef V2
            knobs[0].updateSwitch(cols[0]); // C0: Knob 0 S
            knobs[3].updateSwitch(cols[1]); // C1: Knob 3 S
        #endif
    } else if (rowIdx == 6) {
        eastConnected = (cols[3] == 0);
        #ifdef V1
            knobs[0].updateSwitch(cols[0]); // C0: Knob 1 S
            knobs[1].updateSwitch(cols[1]); // C1: Knob 2 S
        #elifdef V2
            knobs[1].updateSwitch(cols[0]); // C0: Knob 1 S
            knobs[2].updateSwitch(cols[1]); // C1: Knob 2 S 
        #endif
    }
}

void mapColumnsToSet(std::bitset<32> &localInputs, std::bitset<4> cols, uint8_t rowIdx) {
    int offset = rowIdx * 4;
    for (int bit = 0; bit < 4; bit++) localInputs[offset + bit] = cols[bit];
}

void constructAndSendTXMessage(std::bitset<32> &localInputs, std::bitset<32> &prevInputs, uint8_t keyIdx, std::array<uint8_t, 8> &TX_Message) {
    bool isPressed = (localInputs[keyIdx] == 0);
    bool wasPressed = (prevInputs[keyIdx] == 0);

    if (isPressed != wasPressed) {
        if (isPressed) {
            TX_Message[0] = 'P';
            TX_Message[1] = keyIdx;
            TX_Message[2] = (int8_t)knobs[pitchIdx].getValue();
            TX_Message[3] = knobs[waveIdx].getValue();
            TX_Message[4] = knobs[octaveIdx].getValue();
            TX_Message[5] = knobs[volumeIdx].getValue();
        }
        else {
            TX_Message[0] = 'R';
            TX_Message[1] = keyIdx;
        }
        xQueueSend(msgOutQ, TX_Message.data(), 0); // If you spam keys this causes deadlocks if set to portMAX_DELAY
        xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        sysState.TX_Message = TX_Message;
        xSemaphoreGive(sysState.mutex);
    }
}

void updateDisplayState() {
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    OctaveControlMode octaveMode = sysState.octaveMode;
    xSemaphoreGive(sysState.mutex);

    uint8_t displayOctaveIdx = (octaveMode == OCTAVE_LOCAL) ? octaveIdx : octaveOffsetIdx;
    uint16_t activeNotes = 0;
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (sounds[i].active) activeNotes |= (1 << sounds[i].key);
    }

    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    sysState.displayState.waveform = knobs[waveIdx].getValue();
    sysState.displayState.volume = knobs[volumeIdx].getValue();
    sysState.displayState.pitch = knobs[pitchIdx].getValue();
    sysState.displayState.octave = knobs[displayOctaveIdx].getValue();
    sysState.displayState.role = sysState.role;
    sysState.displayState.octaveMode = sysState.octaveMode;    
    sysState.displayState.activeNotes = activeNotes;
    xSemaphoreGive(sysState.mutex);
}  

bool displayStateChanged(const DisplayState &lastState, const DisplayState &currentState, const std::array<uint8_t, 8> &lastMsg, const std::array<uint8_t, 8> &currentMsg) {
    if (lastState.waveform != currentState.waveform ||
        lastState.volume != currentState.volume ||
        lastState.pitch != currentState.pitch ||
        lastState.octave != currentState.octave ||
        lastState.role != currentState.role ||
        lastState.octaveMode != currentState.octaveMode ||
        lastState.activeNotes != currentState.activeNotes) {
        return true;
    }
    return memcmp(lastMsg.data(), currentMsg.data(), 8) != 0;
}

// ================================================= //
// ===================== Tasks ===================== //
// ================================================= //

void scanKeysTask(void * pvParameters) {
    const TickType_t xFrequency = scanInterval/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static std::bitset<32> prevInputs;
    static std::array<uint8_t, 8> TX_Message = {0};
    static uint8_t lastPitch;
    static uint8_t lastVolume;
    static uint8_t lastWaveform;
    static int8_t lastOctave;
    static bool westConnected = false;
    static bool eastConnected = false;

    #ifndef DISABLE_THREADS
        while (1) {
            vTaskDelayUntil( &xLastWakeTime, xFrequency );
    #endif

            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            OctaveControlMode localOctaveMode = sysState.octaveMode;
            xSemaphoreGive(sysState.mutex);

            // Key scanning loop for Rows 0-2
            std::bitset<32> localInputs;
            for (int i = 0; i < 7; i++) { 
                setRow(i);
                delayMicroseconds(3);
                std::bitset<4> cols = readCols();

                updateRotations(i, cols, localOctaveMode); 
                updateSwitchesAndConnections(cols, i, westConnected, eastConnected);
                mapColumnsToSet(localInputs, cols, i);
            }

            #ifdef PROFILE_SCANKEYS
                // WCET: Force 12 messages to be sent every time regardless of actual state
                for (int i = 0; i < 12; i++) {
                    TX_Message[0] = 'P'; // Force "Pressed" status
                    TX_Message[1] = 4;   // Fixed octave
                    TX_Message[2] = i;   // Key index
                    xQueueSend(msgOutQ, TX_Message.data(), 0); // Non-blocking send
                }
            #else
        
                handleSwitches(knobs[volumeIdx].isPressed(), knobs[waveIdx].isPressed(), knobs[octaveIdx].isPressed());
                handleSynthRole(westConnected, eastConnected, knobs[pitchIdx].isPressed());
                xSemaphoreTake(sysState.mutex, portMAX_DELAY);
                SynthRole localRole = sysState.role;
                sysState.inputs = localInputs;    
                xSemaphoreGive(sysState.mutex);

                bool isSender = (localRole == SENDER);
                bool isSingle = (localRole == SINGLE);

                for (int i = 0; i < 12; i++) {
                    if (!isSingle) constructAndSendTXMessage(localInputs, prevInputs, i, TX_Message);
                    
                    if (!isSender) {
                        bool isPressed = (localInputs[i] == 0);
                        bool wasPressed = (prevInputs[i] == 0);

                        if (isPressed && !wasPressed) pushNoteOnCommand(i, knobs[volumeIdx].getValue(), knobs[pitchIdx].getValue(), knobs[waveIdx].getValue(), false, knobs[octaveIdx].getValue());
                        else if (!isPressed && wasPressed) pushNoteOffCommand(i, false);                
                    }
                }

                uint8_t pitch = knobs[pitchIdx].getValue();
                uint8_t volume = knobs[volumeIdx].getValue();
                uint8_t waveform = knobs[waveIdx].getValue();
                uint8_t octave = knobs[octaveIdx].getValue();

                if (pitch != lastPitch) {
                    globalParams.pitch = pitch;
                    lastPitch = pitch;
                }
                if (volume != lastVolume) {
                    globalParams.volume = volume;
                    lastVolume = volume;
                }
                if (waveform != lastWaveform) {
                    globalParams.waveform = (SynthWaveform)waveform;
                    lastWaveform = waveform;
                }
                if (octave != lastOctave) {
                    globalParams.octave = octave;
                    lastOctave = octave;
                }
            #endif
        
            prevInputs = localInputs;

    #ifndef DISABLE_THREADS
        }
    #endif
}

void decodeTask(void * pvParameters) {
    #ifndef DISABLE_THREADS
        std::array<uint8_t, 8> localRX;
    
        while (1) {
            // Block until message available in queue
            xQueueReceive(msgInQ, localRX.data(), portMAX_DELAY);
    #else
        // Initialize with a worst-case index (e.g., key 11)
        std::array<uint8_t, 8> localRX = {'P', 4, 11, 0, 0, 0, 0, 0};
    #endif

            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            SynthRole localRole = sysState.role;
            xSemaphoreGive(sysState.mutex);

            #ifdef PROFILE_DECODE
                int8_t senderOctave = localRX[1];
                uint32_t localStepSize = stepSizes[localRX[2]];
                localStepSize <<= 1; // Force a shift operation

                __atomic_store_n(&currentStepSize, localStepSize, __ATOMIC_RELAXED);
            #else
                if (localRole == RECEIVER) {
                    uint8_t key = localRX[1];
                    if (localRX[0] == 'P') pushNoteOnCommand(key, localRX[5], localRX[2], (SynthWaveform)localRX[3], true, std::clamp(localRX[4] + knobs[octaveOffsetIdx].getValue(), 0, 8)); // Sender octave plus receiver's octave offset, clamped to valid range                
                    else if (localRX[0] == 'R') pushNoteOffCommand(key, true);

                    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
                    sysState.RX_Message = localRX;
                    xSemaphoreGive(sysState.mutex);
                }
            #endif
    #ifndef DISABLE_THREADS
    }
    #endif
}

void CAN_TX_Task (void * pvParameters) {
    #ifndef DISABLE_THREADS
        std::array<uint8_t, 8> msgOut;
        while (1) {
            xQueueReceive(msgOutQ, msgOut.data(), portMAX_DELAY);
            xSemaphoreTake(CAN_TX_Semaphore, portMAX_DELAY);
    #else
        std::array<uint8_t, 8> msgOut = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
    #endif
		CAN_TX(0x123, msgOut.data());
    #ifndef DISABLE_THREADS
        }
    #endif
}

void displayUpdateTask(void * pvParameters) {
    const TickType_t xFrequency = displayInterval/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static std::array<uint8_t, 8> lastMsg = {0};
    static DisplayState lastDisplayState = {};

    #ifndef DISABLE_THREADS
        while (1) { // Standard RTOS mode
            vTaskDelayUntil( &xLastWakeTime, xFrequency );
    #endif
            updateDisplayState();
            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            std::array<uint8_t, 8> receivedMsg = sysState.RX_Message;
            std::array<uint8_t, 8> sentMsg = sysState.TX_Message;
            SynthRole role = sysState.role;
            OctaveControlMode octaveMode = sysState.octaveMode;
            DisplayState displayState = sysState.displayState;
            xSemaphoreGive(sysState.mutex);
            std::array<uint8_t, 8> msg = {0};
        
            //Update display
            u8g2.clearBuffer();                 
            u8g2.setFont(u8g2_font_ncenB08_tr); 
            u8g2.setCursor(2,10);

            #ifdef PROFILE_DISPLAY
                u8g2.print("FFFFFFFF Note: G#"); // Max length string
                u8g2.setCursor(2, 20);
                u8g2.print("K: 8, 8, Oct: 8, Vol: 8"); // Max value strings
                u8g2.setCursor(2,30);
                u8g2.print("FFF"); // Max length string
                u8g2.setCursor(50,30);
                u8g2.print("Role: SENDER");
            #else
                u8g2.print("Notes: ");
                for (int i = 0; i < 12; i++) {
                    if (displayState.activeNotes & (1 << i)) u8g2.print(noteNames[i]);
                }
                
                u8g2.setCursor(2, 20);
                u8g2.print("P: "); 
                u8g2.print(displayState.pitch);
                
                u8g2.print(", W: ");
                u8g2.print(waveNames[displayState.waveform]);
                
                u8g2.print((octaveMode == OCTAVE_OFFSET) ? ", O+:" : ", O:");
                u8g2.print(displayState.octave);
                
                u8g2.print(", V: "); 
                u8g2.print(displayState.volume);
                
                u8g2.setCursor(2, 30);
                u8g2.print("R:");
                u8g2.print(displayState.role == SENDER ? "S" : displayState.role == RECEIVER ? "R" : "1");
                
                if (role != SINGLE) {
                    msg = (role == SENDER) ? sentMsg : receivedMsg;
                    u8g2.print(", ");
                    u8g2.print((char)msg[0]);
                    u8g2.print(msg[1]);
                    u8g2.print(msg[2]);
                    u8g2.print(msg[3]);
                    u8g2.print(msg[4]);
                    u8g2.print(msg[5]);
                }

            #endif

            if (displayStateChanged(lastDisplayState, displayState, lastMsg, msg)) u8g2.sendBuffer();

            lastDisplayState = displayState;
            lastMsg = msg;

            //Toggle LED
            digitalToggle(LED_BUILTIN);

    #ifndef DISABLE_THREADS
        }
    #endif
}

// ================================================= //
// ================= Setup Helpers ================= //
// ================================================= //

void setPinDirections() {
    pinMode(RA0_PIN, OUTPUT);
    pinMode(RA1_PIN, OUTPUT);
    pinMode(RA2_PIN, OUTPUT);
    pinMode(REN_PIN, OUTPUT);
    pinMode(OUT_PIN, OUTPUT);
    pinMode(OUTL_PIN, OUTPUT);
    pinMode(OUTR_PIN, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);

    pinMode(C0_PIN, INPUT);
    pinMode(C1_PIN, INPUT);
    pinMode(C2_PIN, INPUT);
    pinMode(C3_PIN, INPUT);
    pinMode(JOYX_PIN, INPUT);
    pinMode(JOYY_PIN, INPUT);
}

void initialiseDisplay() {
    setOutMuxBit(DRST_BIT, LOW);  //Assert display logic reset
    delayMicroseconds(2);
    setOutMuxBit(DRST_BIT, HIGH);  //Release display logic reset
    u8g2.begin();
    setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply
    setOutMuxBit(KNOB_MODE, HIGH);  //Do read knobs through key matrix
}

void initialiseCANBus() {
    CAN_Init(true);
    setCANFilter(0x123,0x7ff);

    #ifndef DISABLE_ISRS
        CAN_RegisterRX_ISR(CAN_RX_ISR);
        CAN_RegisterTX_ISR(CAN_TX_ISR);
    #endif
    
    CAN_Start();

    msgInQ = xQueueCreate(36, 8);
    msgOutQ = xQueueCreate(384, 8); // Increased to hold 32 iterations of 12 key messages

    CAN_TX_Semaphore = xSemaphoreCreateCounting(3,3);
}

void initialiseKnobs() {
    sysState.mutex = xSemaphoreCreateMutex();
    
    for (int i = 0; i < 5; i++) knobs[i].begin();
    
    for (int i = 3; i < 5; i++) {
        setRow(i);
        delayMicroseconds(3);
        std::bitset<4> cols = readCols();
        
        #ifdef V1
            uint8_t knobIndex = (i == 3) ? 3 : 1;
            knobs[knobIndex].setInitialState(cols[0],cols[1]);
            knobs[knobIndex-1].setInitialState(cols[2],cols[3]);
            if (i == 3) knobs[4].setInitialState(cols[2], cols[3]);
        #elifdef V2
            if (i == 3) {
                knobs[3].setInitialState(cols[0], cols[1]);
                knobs[0].setInitialState(cols[2], cols[3]);
            }
            if (i == 4) {
                knobs[2].setInitialState(cols[0], cols[1]);
                knobs[4].setInitialState(cols[0], cols[1]);
                knobs[1].setInitialState(cols[2], cols[3]);
            }
        #endif
    }
}

void initialiseHardwareTimer() {
    sampleTimer.setOverflow(22000, HERTZ_FORMAT);

    #ifndef DISABLE_ISRS
        sampleTimer.attachInterrupt(sampleISR);
    #endif

    sampleTimer.resume();
}

void initialiseThreads() {
    #ifndef DISABLE_THREADS
        TaskHandle_t scanKeysHandle = NULL;
        TaskHandle_t decodeHandle = NULL;
        TaskHandle_t displayUpdateHandle = NULL;
        TaskHandle_t canTxHandle = NULL;
        xTaskCreate(scanKeysTask, "scanKeys", 256, NULL, 3, &scanKeysHandle);
        xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);
        xTaskCreate(decodeTask, "decode", 256, NULL, 2, &decodeHandle);
        xTaskCreate(CAN_TX_Task, "canTX", 128, NULL, 4, &canTxHandle);
    #endif
}

void initSoundAllocator() {
    for (uint8_t i = 0; i < MAX_SOUNDS; i++) freeSounds[i] = i;
    freeTop = MAX_SOUNDS;
}

// ================================================= //
// ===================== Setup ===================== //
// ================================================= //

void setup() {
    // put your setup code here, to run once:

    //Set pin directions
    setPinDirections();

    //Initialise display
    initialiseDisplay();

    //Initialise UART
    Serial.begin(9600);
    Serial.println("Hello World");

    //Initialise CAN bus
    initialiseCANBus();

    //Initialise PCAL6408A (using i2cMutex)
    initialiseKnobs();

    // Initialize the sounds
    initSoundAllocator();

    // Initialise hardware timer
    initialiseHardwareTimer();

    //Initialise and run threads
    initialiseThreads();

    #ifndef DISABLE_THREADS
        //Start RTOS scheduler
        vTaskStartScheduler();
    #endif
}

// ================================================= //
// ===================== Loop ====================== //
// ================================================= //

void loop() {
    #ifdef PROFILING_MODE
        uint32_t startTime = 0;
        uint32_t endTime = 0;
        const int iterations = 32;

        #ifdef PROFILE_SCANKEYS
            xQueueReset(msgOutQ);
            startTime = micros();

            for(int i = 0; i < iterations; i++) scanKeysTask(NULL);
            
            endTime = micros();
            Serial.print("scanKeys ");
        #endif

        #ifdef PROFILE_DISPLAY
            startTime = micros();
            
            for(int i = 0; i < iterations; i++) displayUpdateTask(NULL);
            
            endTime = micros();
            Serial.print("DisplayUpdate ");
        #endif

        #ifdef PROFILE_DECODE
            // Pre-fill the queue so decodeTask has something to "process" even if scheduler is off
            uint8_t dummyMsg[8] = {'P', 4, 1, 0, 0, 0, 0, 0};
            
            for(int i = 0; i < iterations; i++) xQueueSend(msgInQ, dummyMsg, 0);

            startTime = micros();
            
            for(int i = 0; i < iterations; i++) decodeTask(NULL);

            endTime = micros();
            Serial.print("DecodeTask ");
        #endif

        #ifdef PROFILE_KNOB
            startTime = micros();
            
            for(int i = 0; i < iterations; i++) knobTask(NULL);
            
            endTime = micros();
            Serial.print("KnobTask ");
        #endif

        #ifdef PROFILE_CAN_TX
            startTime = micros();
            
            for(int i = 0; i < iterations; i++) CAN_TX_Task(NULL);

            endTime = micros();
            Serial.print("CAN_TX_Task ");
        #endif

        #ifdef PROFILE_SAMPLE_ISR
            startTime = micros();

            for(int i = 0; i < iterations; i++) sampleISR();

            endTime = micros();
            Serial.print("SampleISR ");
        #endif

        #ifdef PROFILE_CAN_RX_ISR
            xQueueReset(msgInQ);
            startTime = micros();

            for(int i = 0; i < iterations; i++) CAN_RX_ISR();

            endTime = micros();
            Serial.print("CAN_RX_ISR ");
        #endif

        #ifdef PROFILE_CAN_TX_ISR
            vSemaphoreDelete(CAN_TX_Semaphore);
            CAN_TX_Semaphore = xSemaphoreCreateCounting(255, 0);
            startTime = micros();
            
            for(int i = 0; i < iterations; i++) CAN_TX_ISR();
            
            endTime = micros();
            Serial.print("CAN_TX_ISR ");
        #endif

        #ifdef PROFILE_KNOB_ISR
            xSemaphoreTake(knobSemaphore, 0);
            startTime = micros();
            
            for(int i = 0; i < iterations; i++) knobISR();

            endTime = micros();
            Serial.print("KnobISR ");
        #endif

        float totalTime = endTime - startTime;
        Serial.print("Average WCET: ");
        Serial.print(totalTime / iterations);
        Serial.println(" us");

        while(1); // Stop execution
    #endif
}