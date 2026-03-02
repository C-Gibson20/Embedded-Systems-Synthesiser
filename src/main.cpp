#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <STM32FreeRTOS.h>
#include <Wire.h>
#include <ES_CAN.h>
#include <bits/stdc++.h>
#include "Knob.h"
#include "sine_lut.h"

// ================================================= //
// ==================== Versions =================== //
// ================================================= //

// #define V1
#define V2

#ifdef V2
    #define I2C_EXPANDER_KNOBS
#endif 

#ifdef I2C_EXPANDER_KNOBS
  SemaphoreHandle_t i2cMutex; 
  SemaphoreHandle_t knobSemaphore;
  const int EXPANDER_INT_PIN = PA10;
#endif

// ================================================= //
// =================== Profiling =================== //
// ================================================= //

#define PROFILING_MODE  
#ifdef PROFILING_MODE
    #define DISABLE_THREADS
    #define DISABLE_ISRS
  
    #define PROFILE_SCANKEYS
    #define PROFILE_DISPLAY
    #define PROFILE_DECODE
    #define PROFILE_CAN_TX

    #define PROFILE_SAMPLE_ISR
    #define PROFILE_CAN_RX_ISR
    #define PROFILE_CAN_TX_ISR

    #ifdef I2C_EXPANDER_KNOBS
        #define PROFILE_KNOB
        #define PROFILE_KNOB_ISR
    #endif
#endif

// ================================================= //
// ============ Direct Port Manipulation =========== //
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

// ================================================= //
// =================== Constants =================== //
// ================================================= //

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

//Knobs
const uint8_t pitchIdx = 0;
const uint8_t waveIdx = 1;
const uint8_t octaveIdx = 2;
const uint8_t volumeIdx = 3;
const uint8_t octaveOffsetIdx = 4;

//Sounds
const int MAX_SOUNDS = 16;
constexpr int AUDIO_COMMAND_QUEUE_LENGTH = 32;

// ================================================= //
// ================== Shared State ================= //
// ================================================= //

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

SemaphoreHandle_t CAN_TX_Semaphore;

//Knobs
Knob knobs[5] = {
    Knob(0, -128, 127),
    Knob(0, 0, 5),
    Knob(4, 0, 8), // Local octave control
    Knob(2, 0, 8),
    Knob(0, -8, 8) // Octave offset for RECEIVER role when in OCTAVE_OFFSET mode 
};

//Sounds
enum AudioCommandType {NOTE_ON, NOTE_OFF, HOLD_ON, HOLD_OFF, ROLE_CHANGE}; 

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
    bool remote; 
};

struct GlobalParameters {
    volatile uint8_t volume;
    volatile SynthWaveform waveform;
    volatile uint8_t octave;
    volatile int32_t pitch;
    volatile bool hasChanged;
};

struct AudioCommand {
    AudioCommandType type;
    SynthRole newRole;
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

AudioCommand audioCommandQueue[AUDIO_COMMAND_QUEUE_LENGTH];
volatile uint8_t audioCommandWriteIdx = 0;
volatile uint8_t audioCommandReadIdx = 0;

// ================================================= //
// ================ Pin Definitions ================ //
// ================================================= //

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

//CAN Bus Communication
QueueHandle_t msgInQ;
QueueHandle_t msgOutQ;

// ================================================= //
// ================ Hardware Helpers =============== //
// ================================================= //

#ifdef I2C_EXPANDER_KNOBS

    void wireWrites(uint8_t enableAddress, uint8_t writeVal) {
        Wire.beginTransmission(EXPANDER_ADDR);
        Wire.write(enableAddress);
        Wire.write(writeVal);
        Wire.endTransmission();
    }

    extern "C" uint8_t u8x8_byte_rtos_hw_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_init, void *arg_ptr) {
        uint8_t *data;
        switch (msg) {
            case U8X8_MSG_BYTE_SEND:
                data = (uint8_t *)arg_ptr;
                while (arg_init > 0) {
                    Wire.write((uint8_t)*data);
                    data++;
                    arg_init--;
                }
                break;
            case U8X8_MSG_BYTE_INIT:
                // Wire.begin() already initialised in setup
                break;
            case U8X8_MSG_BYTE_SET_DC:
                // Not used for I2C display
                break;
            case U8X8_MSG_BYTE_START_TRANSFER:
                xSemaphoreTake(i2cMutex, portMAX_DELAY);
                Wire.beginTransmission(u8x8_GetI2CAddress(u8x8) >> 1);
                break;
            case U8X8_MSG_BYTE_END_TRANSFER:
                Wire.endTransmission();
                xSemaphoreGive(i2cMutex);
                break;
        }
        return 1;
    }

#endif

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

    #ifdef I2C_EXPANDER_KNOBS 
        if (rowIdx == 2) OUT_LOW(); else OUT_HIGH();
    #else
        OUT_HIGH();
    #endif

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

uint32_t funcSquare(uint8_t i) { 
    return (i < 128) ? 255 : 0; 
}

uint32_t funcSaw(uint8_t i) { 
    return i; 
}

uint32_t funcTri(uint8_t i) { 
    return (i < 128) ? (i << 1) : (511 - (i << 1)); 
}

uint32_t funcSine(uint8_t i) { 
    return sineTable[i]; 
}

uint32_t funcSuperSaw(uint8_t index) {
    uint8_t saw1 = index;
    uint8_t saw2 = (index + (index >> 2)) & 0xFF; 
    return (saw1 + saw2) >> 1;
}

uint32_t funcSineFold(uint8_t index) {
    int16_t val = (sineTable[index] - 128) * 2; 
    if (val > 127) val = 255 - val;             
    if (val < -128) val = -255 - val;           
    return val + 128;
}

typedef uint32_t (*WaveformFunc)(uint8_t index);
const WaveformFunc waveTable[] = {
    funcSquare,   // 0: SQUARE
    funcSaw,      // 1: SAW
    funcTri,      // 2: TRIANGLE
    funcSine,     // 3: SINE
    funcSuperSaw, // 4: SUPERSAW
    funcSineFold  // 5: SINEFOLD
};

// ================================================= //
// ============= Interrupt Subroutines ============= //
// ================================================= //

#ifdef I2C_EXPANDER_KNOBS

    void knobISR() {
        #ifdef PROFILING_MODE
            // Use standard API to prevent RTOS context crashes in main loop    
            xSemaphoreGive(knobSemaphore);
        #else
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(knobSemaphore, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        #endif
    }

#endif

void sampleISR() {
    processAudioCommands();

    if (globalParams.hasChanged) {
        globalParams.hasChanged = false;
        for (int i = 0; i < MAX_SOUNDS; i++) {
            if (sounds[i].active && !sounds[i].held && !sounds[i].remote) {
                sounds[i].volume = globalParams.volume;
                sounds[i].waveform = globalParams.waveform;
                sounds[i].pitch = globalParams.pitch;   

                uint32_t baseStep = computeStep(sounds[i].key, globalParams.octave);
                int32_t offset = (baseStep * (globalParams.pitch >> 2)) >> 6;
                sounds[i].effectiveStep = baseStep + offset;
            }
        }
    }

    int32_t mixedVout = 0;
    uint8_t activeNotes = 0;
    
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if(!sounds[i].active) continue;

        sounds[i].phase += sounds[i].effectiveStep;
        uint8_t index = sounds[i].phase >> 24;

        uint8_t waveIdx = (uint8_t)sounds[i].waveform;
        if (waveIdx > 5) waveIdx = 0;

        uint32_t uncenteredValue = waveTable[waveIdx](index);

        int32_t noteVout = (int32_t)(uncenteredValue) - 128;
        noteVout >>= (8 - sounds[i].volume); // Apply volume control
        mixedVout += noteVout;
        activeNotes++;
    }

    static const uint16_t invGain[] = {0, 256/1, 256/2, 256/3, 256/4, 256/5, 256/6, 256/7, 256/8, 256/9, 256/10, 256/11, 256/12, 256/13, 256/14, 256/15, 256/16};

    if (activeNotes > 1) {
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
        // Use standard API to prevent RTOS context crashes in main loop
        xQueueSend(msgInQ, RX_Message_ISR.data(), 0);
    #else
        CAN_RX(ID, RX_Message_ISR.data());
        xQueueSendFromISR(msgInQ, RX_Message_ISR.data(), NULL);
    #endif
}

void CAN_TX_ISR (void) {
	#ifdef PROFILING_MODE
        // Use standard API to prevent RTOS context crashes in main loop
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

void handleSynthRole(SynthRole &localRole, bool westConnected, bool eastConnected, bool pitchPressed) {
    static SynthRole lastRole = SINGLE;
    static bool overwrittenAutoConfig = false;

    // Disconnected defaults to single mode and resets auto-config override
    if (!westConnected && !eastConnected) {
        localRole = SINGLE;
        overwrittenAutoConfig = false;
    }
    
    // Manual override if connected to at least one other device
    else if (pitchPressed) {
        overwrittenAutoConfig = true;
        localRole = (localRole == SENDER) ? RECEIVER : SENDER;
    }

    // If auto-configuration has not been overridden, determine role based on connections
    else if (!overwrittenAutoConfig && !westConnected && eastConnected) localRole = SENDER;
    else if (!overwrittenAutoConfig && westConnected) localRole = RECEIVER;

    // role = RECEIVER; // Force receiver for testing

    if (localRole != lastRole) pushRoleChangeCommand(localRole);
    lastRole = localRole;
}

void updateRotations(uint8_t rowIdx, std::bitset<4> cols, OctaveControlMode localOctaveMode) {
    #ifdef V1
        if (rowIdx == 3) {
            knobs[3].updateRotation(cols[0], cols[1]);
            
            if (localOctaveMode == OCTAVE_LOCAL) {
                knobs[2].updateRotation(cols[2], cols[3]);
                knobs[4].setInitialState(cols[2], cols[3]); // Sync octave offset knob with local octave
            } else {
                knobs[4].updateRotation(cols[2], cols[3]);
                knobs[2].setInitialState(cols[2], cols[3]); // Sync local octave knob with octave offset
            }
        } else if (rowIdx == 4) {
            knobs[1].updateRotation(cols[0], cols[1]);
            knobs[0].updateRotation(cols[2], cols[3]);
        }
    #elifdef V2
        if (rowIdx == 3) {
            knobs[3].updateRotation(cols[0], cols[1]);
            knobs[0].updateRotation(cols[2], cols[3]);
        }
        if (rowIdx == 4) {
            if (localOctaveMode == OCTAVE_LOCAL){
                knobs[2].updateRotation(cols[0], cols[1]);
                knobs[4].setInitialState(cols[0], cols[1]); // Sync octave offset knob with local octave
            } else {
                knobs[4].updateRotation(cols[0], cols[1]);
                knobs[2].setInitialState(cols[0], cols[1]); // Sync local octave knob with octave offset
            }
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
        #ifdef V1
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

void updateGlobalParams(uint32_t &lastPitch, uint8_t &lastVolume, uint8_t &lastWaveform, uint8_t &lastOctave, uint32_t pitch, uint8_t volume, uint8_t waveform, uint8_t octave) {
    lastPitch = pitch;
    lastVolume = volume;
    lastWaveform = waveform;
    lastOctave = octave;

    globalParams.pitch = pitch;
    globalParams.volume = volume;
    globalParams.waveform = (SynthWaveform)waveform;    
    globalParams.octave = octave;

    __DMB(); // Ensure all parameter updates are visible before setting hasChanged

    globalParams.hasChanged = true;
}

// ================================================= //
// ===================== Tasks ===================== //
// ================================================= //

#ifdef I2C_EXPANDER_KNOBS

    void knobTask(void * pvParameters) {
        #ifndef DISABLE_THREADS
            while (1) {
                xSemaphoreTake(knobSemaphore, portMAX_DELAY);
        #endif
                xSemaphoreTake(sysState.mutex, portMAX_DELAY);
                OctaveControlMode octaveMode = sysState.octaveMode;
                xSemaphoreGive(sysState.mutex);

                int retryCount = 0;
                do {
                    // Safely claim the I2C bus
                    xSemaphoreTake(i2cMutex, portMAX_DELAY);
                    Wire.beginTransmission(EXPANDER_ADDR);
                    Wire.write(REG_INPUT);
                    Wire.endTransmission();
                    Wire.requestFrom(EXPANDER_ADDR, (uint8_t)1);
                    uint8_t currByte = Wire.read();
                    xSemaphoreGive(i2cMutex);

                    #ifdef PROFILE_KNOB
                        // WCET: Force the Knob state machines to process a physical turn
                        // by alternating all bits each iteration.
                        static uint8_t fakeByte = 0x00;
                        fakeByte ^= 0xFF; 
                        currByte = fakeByte;
                    #endif

                    for (int i = 0; i < 4; i++) {
                        if (i == 2) continue;
                        uint8_t bitA = (currByte >> (i * 2)) & 0x01;
                        uint8_t bitB = (currByte >> (i * 2 + 1)) & 0x01;
                        knobs[i].updateRotation(bitA, bitB);
                    }

                    // Context dependent mapping of octave and octave offset knob
                    uint8_t bitA = (currByte >> 4) & 0x01;
                    uint8_t bitB = (currByte >> 5) & 0x01;

                    if (octaveMode == OCTAVE_LOCAL) {
                        knobs[2].updateRotation(bitA, bitB);
                        knobs[4].setInitialState(bitA, bitB); // Sync octave offset knob with local octave
                    } else {
                        knobs[4].updateRotation(bitA, bitB);
                        knobs[2].setInitialState(bitA, bitB); // Sync local octave knob with octave offset
                    }

                    retryCount++;
                    #ifndef PROFILE_KNOB
                        // Yield briefly to let other equal/higher priority tasks run if stuck
                        vTaskDelay(pdMS_TO_TICKS(1)); 
                    #endif
                    
            #ifdef PROFILE_KNOB
                // WCET: Force the worst-case 3 loop iterations
                } while (retryCount < 3); 
            #else
                } while (digitalRead(EXPANDER_INT_PIN) == LOW && retryCount < 3);
            #endif

        #ifndef DISABLE_THREADS
            }
        #endif
    }

#endif

void scanKeysTask(void * pvParameters) {
    const TickType_t xFrequency = scanInterval/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static std::bitset<32> prevInputs;
    static std::array<uint8_t, 8> TX_Message = {0};
    static uint32_t lastPitch;
    static uint8_t lastVolume;
    static uint8_t lastWaveform;
    static uint8_t lastOctave;
    static bool westConnected = false;
    static bool eastConnected = false;

    #ifndef DISABLE_THREADS
        while (1) {
            vTaskDelayUntil( &xLastWakeTime, xFrequency );
    #endif

            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            OctaveControlMode localOctaveMode = sysState.octaveMode;
            SynthRole localRole = sysState.role;
            xSemaphoreGive(sysState.mutex);

            // Key scanning loop for Rows 0-2
            std::bitset<32> localInputs;
            for (int i = 0; i < 7; i++) { 
                setRow(i);
                delayMicroseconds(3);
                std::bitset<4> cols = readCols();

                #ifndef I2C_EXPANDER_KNOBS
                    updateRotations(i, cols, localOctaveMode); 
                #endif 

                updateSwitchesAndConnections(cols, i, westConnected, eastConnected);
                mapColumnsToSet(localInputs, cols, i);
            }

            #ifdef PROFILE_SCANKEYS
                // WCET: Force the worst-case path (RECEIVER role with all keys changing state).
                // Force all 12 keys to trigger a "pressed" state change
                for (int i = 0; i < 12; i++) {
                    localInputs[i] = 0; 
                    prevInputs[i] = 1; 
                }
                
                // Force RECEIVER role (executes BOTH the CAN TX and Audio paths)
                westConnected = true;
                eastConnected = false;
                
                // Force a global parameter update
                lastPitch = knobs[pitchIdx].getValue() + 1;
                
                // Clear queues so they don't overflow during the 32 iterations
                xQueueReset(msgOutQ);
                audioCommandWriteIdx = 0;
                audioCommandReadIdx = 0;
            #else
        
                handleSwitches(knobs[volumeIdx].isPressed(), knobs[waveIdx].isPressed(), knobs[octaveIdx].isPressed());
                handleSynthRole(localRole, westConnected, eastConnected, knobs[pitchIdx].isPressed());

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

                uint32_t pitch = knobs[pitchIdx].getValue();
                uint8_t volume = knobs[volumeIdx].getValue();
                uint8_t waveform = knobs[waveIdx].getValue();
                uint8_t octave = knobs[octaveIdx].getValue();

                if (pitch != lastPitch || volume != lastVolume || waveform != lastWaveform || octave != lastOctave) updateGlobalParams(lastPitch, lastVolume, lastWaveform, lastOctave, pitch, volume, waveform, octave);
            #endif
        
            prevInputs = localInputs;
            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            sysState.role = localRole;
            sysState.inputs = localInputs;    
            xSemaphoreGive(sysState.mutex);

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
        // WCET: Initialize with worst-case payload (Note On, Max Key, Max Pitch, Max Vol)
        std::array<uint8_t, 8> localRX = {'P', 11, 127, 5, 8, 255, 0, 0};
    #endif

            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            SynthRole localRole = sysState.role;
            xSemaphoreGive(sysState.mutex);

            #ifdef PROFILE_DECODE
                // WCET: Force the worst-case path (RECEIVER role processing a NOTE_ON)
                // This forces math (computeStep), clamping, array lookups, and critical section queue pushing.
                uint8_t key = localRX[1];
                pushNoteOnCommand(key, localRX[5], localRX[2], (SynthWaveform)localRX[3], true, std::clamp(localRX[4] + knobs[octaveOffsetIdx].getValue(), 0, 8)); 
                
                xSemaphoreTake(sysState.mutex, portMAX_DELAY);
                sysState.RX_Message = localRX;
                xSemaphoreGive(sysState.mutex);
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
                // WCET: Force the maximum number of pixels to render
                u8g2.print("Notes: CC#DD#EFF#GG#AA#B"); // All 12 notes active
                u8g2.setCursor(2, 20);
                u8g2.print("P: -128, W: SF, O+: 8, V: 8");         // Max character widths
                u8g2.setCursor(2,30);
                u8g2.print("R:S, P2558");                          // Max CAN message width
                
                // WCET: Force the I2C transaction every iteration
                u8g2.sendBuffer();
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

                if (displayStateChanged(lastDisplayState, displayState, lastMsg, msg)) {
                    u8g2.sendBuffer();
                }
                
            #endif

            lastDisplayState = displayState;
            lastMsg = msg;

            //Toggle LED
            digitalToggle(LED_BUILTIN);

    #ifndef DISABLE_THREADS
        }
    #endif
}

// ================================================= //
// ============ Setup and Loop Helpers ============= //
// ================================================= //

#ifdef I2C_EXPANDER_KNOBS

    void clearInterruptAndSync(uint8_t address, uint8_t writeVal) {
        Wire.beginTransmission(address);
        Wire.write(writeVal);
        Wire.endTransmission();
        Wire.requestFrom(address, (uint8_t)1); // Dummy read to clear interrupt
        if (Wire.available()) {
            uint8_t startByte = Wire.read();
            for (int i = 0; i < 4; i++) {
                uint8_t bitA = (startByte >> (i * 2)) & 0x01;
                uint8_t bitB = (startByte >> (i * 2 + 1)) & 0x01;
                knobs[i].setInitialState(bitA, bitB);
                if (i == 2) knobs[4].setInitialState(bitA, bitB); // Sync octave offset knob with local octave on startup
            }
        }
    }

    void initialisePCAL6408A() {
        i2cMutex = xSemaphoreCreateMutex();
        knobSemaphore = xSemaphoreCreateBinary();

        pinMode(EXPANDER_INT_PIN, INPUT_PULLUP);

        Wire.begin();
        wireWrites(REG_PULL_EN, 0xFF); // Enable pull-ups on all pins
        wireWrites(REG_LAT_EN, 0xFF); // Enable latched output for all pins
        wireWrites(REG_INT_MASK, 0x00); // Enable interrupts on all pins

        clearInterruptAndSync(EXPANDER_ADDR, 0x00);

        #ifndef DISABLE_ISRS
            attachInterrupt(digitalPinToInterrupt(EXPANDER_INT_PIN), knobISR, FALLING);
        #endif
    }

    #endif

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

    #ifdef I2C_EXPANDER_KNOBS
        u8g2.getU8x8()->byte_cb = u8x8_byte_rtos_hw_i2c;
    #endif

    u8g2.begin();
    Wire.setClock(1000000); // Increase I2C clock speed for faster display updates

    setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply
}

void initialiseCANBus() {
    #ifdef PROFILING_MODE
        CAN_Init(true);
    #else
        CAN_Init(false);
    #endif
    setCANFilter(0x123,0x7ff);

    #ifndef DISABLE_ISRS
        CAN_RegisterRX_ISR(CAN_RX_ISR);
        CAN_RegisterTX_ISR(CAN_TX_ISR);
    #endif
    
    CAN_Start();

    msgInQ = xQueueCreate(36, 8);
    msgOutQ = xQueueCreate(36, 8);
    // msgOutQ = xQueueCreate(384, 8); // Increased to hold 32 iterations of 12 key messages

    CAN_TX_Semaphore = xSemaphoreCreateCounting(3,3);
}

void initialiseKnobs() {
    #ifdef I2C_EXPANDER_KNOBS
        setOutMuxBit(KNOB_MODE, LOW);  //Do not read knobs through key matrix
    #else 
        setOutMuxBit(KNOB_MODE, HIGH);  //Do read knobs through key matrix
    #endif

    sysState.mutex = xSemaphoreCreateMutex();
    
    for (int i = 0; i < 5; i++) knobs[i].begin();
    
    #ifndef I2C_EXPANDER_KNOBS
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
    #endif
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
        #ifdef I2C_EXPANDER_KNOBS
            TaskHandle_t knobHandle = NULL;
            xTaskCreate(knobTask, "knobTask", 256, NULL, 2, &knobHandle);
        #endif

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
    for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
        freeSounds[i] = i;
        sounds[i].active = false;
        sounds[i].waveform = SQUARE;
        sounds[i].volume = 0;
        sounds[i].phase = 0;
    }
    freeTop = MAX_SOUNDS;
}

void printAverageTime(const char* taskName, uint32_t totalTime, int iterations) {
    Serial.print(taskName);
    Serial.print(" Average WCET: ");
    Serial.print((float)totalTime / iterations);
    Serial.println(" us");
}

void profileTask(void (*taskFunction)(void*), const char* taskName, const int iterations = 32, bool delayBetweenIterations = false) {
    uint32_t totalTime = 0;

    for(int i = 0; i < iterations; i++) {
        if (delayBetweenIterations) delay(2); // Give hardware time to complete operations between iterations

        uint32_t start = micros();
        taskFunction(NULL); 
        uint32_t end = micros();
        totalTime += (end - start);
    }

    printAverageTime(taskName, totalTime, iterations);
}

void profileISR(void (*isrFunction)(void), const char* taskName, const int iterations = 32, bool binarySemaphore = false, bool isSampleISR = false) {
    uint32_t totalTime = 0;

    for(int i = 0; i < iterations; i++) {
        #ifdef I2C_EXPANDER_KNOBS
            if (binarySemaphore) xSemaphoreTake(knobSemaphore, 0); // Empty the binary semaphore so the give is not rejected
        #endif
        if (isSampleISR) {
            // Force the parameters update loop to recalculate steps for all 16 sounds
            // And inject a dummy command to simulate queue processing overhead.
            // Dummy key so NOTE_OFF safely forces a full array search without killing active sounds
            globalParams.hasChanged = true;
            pushNoteOffCommand(255, false); 
        }

        uint32_t start = micros();
        isrFunction();
        uint32_t end = micros();
        totalTime += (end - start);
    }

    printAverageTime(taskName, totalTime, iterations);
}

// ================================================= //
// ===================== Setup ===================== //
// ================================================= //

void setup() {
    // put your setup code here, to run once:

    //Set pin directions
    setPinDirections();

    //Initialise Knobs
    initialiseKnobs();
    #ifdef I2C_EXPANDER_KNOBS
        initialisePCAL6408A();
    #endif

    //Initialise display
    initialiseDisplay();

    //Initialise UART
    Serial.begin(9600);
    Serial.println("Hello World");

    //Initialise CAN bus
    initialiseCANBus();

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
        const int iterations = 32;

        #ifdef PROFILE_SCANKEYS
            profileTask(scanKeysTask, "scanKeysTask");
        #endif

        #ifdef PROFILE_DISPLAY
            profileTask(displayUpdateTask, "displayUpdateTask");
        #endif

        #ifdef PROFILE_DECODE
            // Reset the audio command queue so pushNoteOnCommand writes to memory instead of skipping because the queue is full.
            audioCommandWriteIdx = 0;
            audioCommandReadIdx = 0;
            profileTask(decodeTask, "decodeTask");
        #endif

        #ifdef PROFILE_KNOB
            profileTask(knobTask, "knobTask");
        #endif

        #ifdef PROFILE_CAN_TX
            profileTask(CAN_TX_Task, "CAN_TX_Task", iterations, true); // Add delay between iterations
        #endif

        #ifdef PROFILE_SAMPLE_ISR
            // WCET Setup: Force maximum polyphony
            for (int i = 0; i < MAX_SOUNDS; i++) {
                sounds[i].active = true;
                sounds[i].held = false;
                sounds[i].remote = false;
                sounds[i].waveform = SINEFOLD; // Most computationally expensive waveform
                sounds[i].key = i % 12;
                sounds[i].pitch = 127;
                sounds[i].volume = 0; 
            }

            profileISR(sampleISR, "sampleISR", iterations, false, true);
        #endif

        #ifdef PROFILE_CAN_RX_ISR
            xQueueReset(msgInQ);
            profileISR(CAN_RX_ISR, "CAN_RX_ISR");
        #endif

        #ifdef PROFILE_CAN_TX_ISR
            vSemaphoreDelete(CAN_TX_Semaphore);
            CAN_TX_Semaphore = xSemaphoreCreateCounting(255, 0);
            profileISR(CAN_TX_ISR, "CAN_TX_ISR");
        #endif

        #ifdef PROFILE_KNOB_ISR
            profileISR(knobISR, "knobISR", iterations, true, false); // Add binary semaphore handling
        #endif

        while(1); // Stop execution
    #endif
}