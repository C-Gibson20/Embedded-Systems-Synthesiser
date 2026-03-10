#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "Display.h"
#include "Knob.h"
#include "constants.h"
#include "pins.h"

U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

// From main.cpp
extern volatile struct Sound sounds[];   // needs Sound struct visible — see note below
extern Knob knobs[];
extern void setOutMuxBit(const uint8_t bitIdx, const bool value);

void updateDisplayState() {
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    OctaveControlMode octaveMode = sysState.octaveMode;
    xSemaphoreGive(sysState.mutex);

    uint8_t displayOctaveIdx = (octaveMode == OCTAVE_LOCAL) ? octaveIdx : octaveOffsetIdx;
    // TODO: data race — sounds[] is written by sampleISR without synchronisation.
    // Fix: read inside a critical section (taskENTER_CRITICAL / taskEXIT_CRITICAL)
    // or maintain a separate ISR-safe active-notes bitmask.
    uint16_t activeNotes = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
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

void displayUpdateTask(void * pvParameters) {
    const TickType_t xFrequency = DISPLAY_INTERVAL/portTICK_PERIOD_MS;
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
                    if (displayState.activeNotes & (1 << i)) u8g2.print(NOTE_NAMES[i]);
                }
                
                u8g2.setCursor(2, 20);
                u8g2.print("P: "); 
                u8g2.print(displayState.pitch);
                
                u8g2.print(", W: ");
                u8g2.print(WAVE_NAMES[displayState.waveform]);
                
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