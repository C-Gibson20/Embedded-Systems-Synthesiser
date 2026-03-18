#include <Arduino.h>
#include <bitset>
#include <STM32FreeRTOS.h>
#include "SysState.h"
#include "ui/Display.h"
#include "constants.h"
#include "pins.h"
#include "io/KeyMatrix.h"
#include "io/KnobManager.h"
#include "audio/Synth.h"
#include "net/CanProtocol.h"
#ifdef DINO_MODE
    #include "ui/DinoGame.h"
#endif

// Version flags (V2, I2C_EXPANDER_KNOBS) are set in platformio.ini build_flags.

// ================================================= //
// =================== Profiling =================== //
// ================================================= //

#ifdef PROFILING_MODE
    #define DISABLE_THREADS
    #define DISABLE_ISRS
  
    #define PROFILE_SAMPLEGEN
    #define PROFILE_SCANKEYS
    #define PROFILE_DISPLAY
    #define PROFILE_DECODE
    #define PROFILE_CAN_TX

    #define PROFILE_SAMPLE_ISR
    #define PROFILE_CAN_RX_ISR
    #define PROFILE_CAN_TX_ISR

    // #ifdef I2C_EXPANDER_KNOBS
    //     #define PROFILE_KNOB
    //     #define PROFILE_KNOB_ISR
    // #endif
#endif

// ================================================= //
// ================== Shared State ================= //
// ================================================= //

SysState sysState;

KeyMatrix matrix;
#ifdef DINO_MODE
DinoGame dinogame;
#endif

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

// ================================================= //
// ===================== Tasks ===================== //
// ================================================= //

void sampleGenTask(void* pvParameters) {
    HAL_DAC_Start_DMA(&hdac1, DAC_CHANNEL_1, 
                    (uint32_t*)synth.sampleBuffer, 
                    Synth::BUFFER_SIZE, 
                    DAC_ALIGN_8B_R);

    #ifndef DISABLE_THREADS
        while (1) {
            xSemaphoreTake(synth.sampleBufferSemaphore, portMAX_DELAY);
    #endif
            synth.fillBuffer();
    #ifndef DISABLE_THREADS
        }
    #endif
}

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
    static uint16_t joyY;

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

            joyY = analogRead(JOYY_PIN);
            //joy neutral 456, joydown 862, joy up 116
            if      (joyY < 300) sysState.joyState = JOY_UP;
            else if (joyY > 700) sysState.joyState = JOY_DOWN;
            else                  sysState.joyState = JOY_NEUTRAL;

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
                xQueueReset(canProtocol.msgOutQ);
                synth.resetCommandQueue();
            #else
        
                handleSwitches(knobManager.knobs[volumeIdx].isPressed(), knobManager.knobs[waveIdx].isPressed(), knobManager.knobs[octaveIdx].isPressed());
                handleSynthRole(localRole, westConnected, eastConnected, knobManager.knobs[pitchIdx].isPressed());

                bool isSender = (localRole == SENDER);
                bool isSingle = (localRole == SINGLE);

                uint32_t pitch   = knobManager.knobs[pitchIdx].getValue();
                uint8_t  volume  = knobManager.knobs[volumeIdx].getValue();
                uint8_t  waveform = knobManager.knobs[waveIdx].getValue();
                uint8_t  octave  = knobManager.knobs[octaveIdx].getValue();

                for (int i = 0; i < 12; i++) {
                    if (!isSingle) canProtocol.handleKeyChange(localInputs, prevInputs, i, TX_Message, (int8_t)pitch, waveform, octave, volume);

                    if (!isSender) {
                        bool isPressed  = (localInputs[i] == 0);
                        bool wasPressed = (prevInputs[i] == 0);

                        if (isPressed && !wasPressed) synth.pushNoteOn(i, volume, pitch, waveform, false, octave);
                        else if (!isPressed && wasPressed) synth.pushNoteOff(i, false);
                    }
                }

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

#ifdef DINO_MODE
void dinoTask(void* pvParameters) {
    const TickType_t xFrequency = 60 / portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    dinoGame.begin();
    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        JoyState joy = sysState.joyState;
        dinoGame.tick(joy);
        display.update();
    }
}
#endif

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

void initialiseThreads() {
    #ifndef DISABLE_THREADS
        #ifdef I2C_EXPANDER_KNOBS
            TaskHandle_t knobHandle = NULL;
            xTaskCreate(knobTask, "knobTask", 256, NULL, 2, &knobHandle);
        #endif
        TaskHandle_t sampleGenHandle = NULL;
        TaskHandle_t scanKeysHandle = NULL;
        TaskHandle_t decodeHandle = NULL;
        TaskHandle_t displayUpdateHandle = NULL;
        TaskHandle_t dinoHandle = NULL;
        TaskHandle_t canTxHandle = NULL;
        xTaskCreate(sampleGenTask, "sampleGen", 256, NULL, 4, &sampleGenHandle);
        xTaskCreate(scanKeysTask, "scanKeys", 256, NULL, 3, &scanKeysHandle);
        #ifdef DINO_MODE
            xTaskCreate(dinoTask, "dino", 256, NULL, 1, &dinoHandle);
        #else
            xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);
        #endif
        xTaskCreate(decodeTask, "decode", 256, NULL, 2, &decodeHandle);
        xTaskCreate(CAN_TX_Task, "canTX", 128, NULL, 5, &canTxHandle);
    #endif
}

#ifdef PROFILING_MODE
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
#endif // PROFILING_MODE

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
    canProtocol.begin();

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

        #ifdef PROFILE_SAMPLEGEN
            // WCET Setup: Force max polyphony
            for (int i = 0; i < MAX_VOICES; i++) {
                synth.sounds[i].active   = true;
                synth.sounds[i].held     = false;
                synth.sounds[i].remote   = false;
                synth.sounds[i].waveform = SINEFOLD; // worst-case waveform for profiling
                synth.sounds[i].key      = i % 12;
                synth.sounds[i].pitch    = 127;
                synth.sounds[i].volume   = 0;
            }
            profileTask(sampleGenTask, "sampleGenTask");
        #endif
        
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
            // profileISR(sampleISR, "sampleISR", iterations, false);
        #endif

        #ifdef PROFILE_CAN_RX_ISR
            xQueueReset(canProtocol.msgInQ);
            profileISR(CAN_RX_ISR, "CAN_RX_ISR");
        #endif

        #ifdef PROFILE_CAN_TX_ISR
            vSemaphoreDelete(canProtocol.txSemaphore);
            canProtocol.txSemaphore = xSemaphoreCreateCounting(255, 0);
            profileISR(CAN_TX_ISR, "CAN_TX_ISR");
        #endif

        #ifdef PROFILE_KNOB_ISR
            profileISR(knobISR, "knobISR", iterations, true); // Add binary semaphore handling
        #endif

        while(1); // Stop execution
    #endif
}