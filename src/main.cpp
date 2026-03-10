#include <Arduino.h>
#include <bitset>
#include <STM32FreeRTOS.h>
#include <Wire.h>
#include <ES_CAN.h>
#include <bits/stdc++.h>
#include "Knob.h"
#include "SysState.h"
#include "ui/Display.h"
#include "constants.h"
#include "pins.h"
#include "io/KeyMatrix.h"
#include "io/KnobManager.h"
#include "audio/Synth.h"

// Version flags (V2, I2C_EXPANDER_KNOBS) are set in platformio.ini build_flags.

// ================================================= //
// =================== Profiling =================== //
// ================================================= //

// #define PROFILING_MODE  
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
// ================== Shared State ================= //
// ================================================= //

SysState sysState;

SemaphoreHandle_t CAN_TX_Semaphore;

KeyMatrix matrix;

//CAN Bus Communication
QueueHandle_t msgInQ;
QueueHandle_t msgOutQ;

// ================================================= //
// ============= Interrupt Subroutines ============= //
// ================================================= //

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

    if (localRole != lastRole) synth.pushRoleChange(localRole);
    lastRole = localRole;
}

void handleSwitches(bool volumePressed, bool wavePressed, bool octavePressed) {
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    bool localHold = sysState.hold;
    SynthRole localRole = sysState.role;
    OctaveControlMode localOctaveMode = sysState.octaveMode;
    xSemaphoreGive(sysState.mutex);

    if (volumePressed) {
        localHold = true;
        synth.pushHold(HOLD_ON);
    }

    if (wavePressed) {
        localHold = false;
        synth.pushHold(HOLD_OFF);
    }

    if (octavePressed && localRole == RECEIVER) localOctaveMode = (localOctaveMode == OCTAVE_LOCAL) ? OCTAVE_OFFSET : OCTAVE_LOCAL;

    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    sysState.hold = localHold;
    sysState.octaveMode = localOctaveMode;
    xSemaphoreGive(sysState.mutex);
}

void constructAndSendTXMessage(std::bitset<32> &localInputs, std::bitset<32> &prevInputs, uint8_t keyIdx, std::array<uint8_t, 8> &TX_Message) {
    bool isPressed = (localInputs[keyIdx] == 0);
    bool wasPressed = (prevInputs[keyIdx] == 0);

    if (isPressed != wasPressed) {
        if (isPressed) {
            TX_Message[0] = 'P';
            TX_Message[1] = keyIdx;
            TX_Message[2] = (int8_t)knobManager.knobs[pitchIdx].getValue();
            TX_Message[3] = knobManager.knobs[waveIdx].getValue();
            TX_Message[4] = knobManager.knobs[octaveIdx].getValue();
            TX_Message[5] = knobManager.knobs[volumeIdx].getValue();
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

// ================================================= //
// ===================== Tasks ===================== //
// ================================================= //

void scanKeysTask(void * pvParameters) {
    const TickType_t xFrequency = SCAN_INTERVAL/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static std::bitset<32> prevInputs;
    static std::array<uint8_t, 8> TX_Message = {0};
    static uint32_t lastPitch;
    static uint8_t lastVolume;
    static uint8_t lastWaveform;
    static uint8_t lastOctave;
    static bool westConnected = false;
    static bool eastConnected = false;
    static int8_t keyToSound[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    #ifndef DISABLE_THREADS
        while (1) {
            vTaskDelayUntil( &xLastWakeTime, xFrequency );
    #endif

            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            OctaveControlMode localOctaveMode = sysState.octaveMode;
            SynthRole localRole = sysState.role;
            xSemaphoreGive(sysState.mutex);

            KeyScanResult scanResult = matrix.scan();
            std::bitset<32> localInputs = scanResult.inputs;
            westConnected = scanResult.westConnected;
            eastConnected = scanResult.eastConnected;

            // Update knob switches from matrix rows 5-6
            #ifdef V1
                knobManager.knobs[2].updateSwitch(scanResult.rowData[5][0]);
                knobManager.knobs[3].updateSwitch(scanResult.rowData[5][1]);
                knobManager.knobs[0].updateSwitch(scanResult.rowData[6][0]);
                knobManager.knobs[1].updateSwitch(scanResult.rowData[6][1]);
            #elifdef V2
                knobManager.knobs[0].updateSwitch(scanResult.rowData[5][0]);
                knobManager.knobs[3].updateSwitch(scanResult.rowData[5][1]);
                knobManager.knobs[1].updateSwitch(scanResult.rowData[6][0]);
                knobManager.knobs[2].updateSwitch(scanResult.rowData[6][1]);
            #endif

            // V1 non-I2C mode: update knob rotations from matrix
            #ifndef I2C_EXPANDER_KNOBS
                for (int i = 0; i < 7; i++) knobManager.updateRotations(i, scanResult.rowData[i], localOctaveMode);
            #endif

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
                lastPitch = knobManager.knobs[pitchIdx].getValue() + 1;

                // Ensure all sounds are "active" for WCET applyGlobalParamUpdates() 
                for (int i = 0; i < MAX_VOICES; i++) {
                    synth.sounds[i].active = true;
                    synth.sounds[i].held = false;
                    synth.sounds[i].remote = false;
                }
                
                // Clear queues so they don't overflow during the 32 iterations
                xQueueReset(msgOutQ);
                synth.resetCommandQueue();
            #else
        
                handleSwitches(knobManager.knobs[volumeIdx].isPressed(), knobManager.knobs[waveIdx].isPressed(), knobManager.knobs[octaveIdx].isPressed());
                handleSynthRole(localRole, westConnected, eastConnected, knobManager.knobs[pitchIdx].isPressed());

                bool isSender = (localRole == SENDER);
                bool isSingle = (localRole == SINGLE);

                for (int i = 0; i < 12; i++) {
                    if (!isSingle) constructAndSendTXMessage(localInputs, prevInputs, i, TX_Message);
                    
                    if (!isSender) {
                        bool isPressed = (localInputs[i] == 0);
                        bool wasPressed = (prevInputs[i] == 0);

                        if (isPressed && !wasPressed) synth.pushNoteOn(i, knobManager.knobs[volumeIdx].getValue(), knobManager.knobs[pitchIdx].getValue(), knobManager.knobs[waveIdx].getValue(), false, knobManager.knobs[octaveIdx].getValue());
                        else if (!isPressed && wasPressed) synth.pushNoteOff(i, false);                
                    }
                }

                uint32_t pitch = knobManager.knobs[pitchIdx].getValue();
                uint8_t volume = knobManager.knobs[volumeIdx].getValue();
                uint8_t waveform = knobManager.knobs[waveIdx].getValue();
                uint8_t octave = knobManager.knobs[octaveIdx].getValue();

                if (pitch != lastPitch || volume != lastVolume || waveform != lastWaveform || octave != lastOctave) {
                    synth.updateGlobalParams(lastPitch, lastVolume, lastWaveform, lastOctave, pitch, volume, waveform, octave);
                    synth.applyGlobalParamUpdates();
                }
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
                synth.pushNoteOn(key, localRX[5], localRX[2], (SynthWaveform)localRX[3], true, std::clamp(localRX[4] + knobManager.knobs[octaveOffsetIdx].getValue(), 0, 8)); 
                
                xSemaphoreTake(sysState.mutex, portMAX_DELAY);
                sysState.RX_Message = localRX;
                xSemaphoreGive(sysState.mutex);
            #else
                if (localRole == RECEIVER) {
                    uint8_t key = localRX[1];
                    if (localRX[0] == 'P') synth.pushNoteOn(key, localRX[5], localRX[2], (SynthWaveform)localRX[3], true, std::clamp(localRX[4] + knobManager.knobs[octaveOffsetIdx].getValue(), 0, 8)); // Sender octave plus receiver's octave offset, clamped to valid range                
                    else if (localRX[0] == 'R') synth.pushNoteOff(key, true);

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

// ================================================= //
// ============ Setup and Loop Helpers ============= //
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

void profileISR(void (*isrFunction)(void), const char* taskName, const int iterations = 32, bool binarySemaphore = false) {
    uint32_t totalTime = 0;

    for(int i = 0; i < iterations; i++) {
        #ifdef I2C_EXPANDER_KNOBS
            if (binarySemaphore) xSemaphoreTake(knobManager.semaphore, 0); // Empty the binary semaphore so the give is not rejected
        #endif

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
    sysState.mutex = xSemaphoreCreateMutex();
    knobManager.begin(matrix);

    //Initialise display
    display.begin();

    //Initialise UART
    Serial.begin(9600);
    Serial.println("Hello World");

    //Initialise CAN bus
    initialiseCANBus();

    // Initialise synth (sound allocator + hardware timer)
    synth.begin();

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
            synth.resetCommandQueue();
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
            for (int i = 0; i < MAX_VOICES; i++) {
                synth.sounds[i].active = true;
                synth.sounds[i].held = false;
                synth.sounds[i].remote = false;
                synth.sounds[i].waveform = SINEFOLD; // Most computationally expensive waveform
                synth.sounds[i].key = i % 12;
                synth.sounds[i].pitch = 127;
                synth.sounds[i].volume = 0;
            }

            profileISR(sampleISR, "sampleISR", iterations, false);
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
            profileISR(knobISR, "knobISR", iterations, true); // Add binary semaphore handling
        #endif

        while(1); // Stop execution
    #endif
}