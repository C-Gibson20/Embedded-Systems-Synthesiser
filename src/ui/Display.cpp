#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "Display.h"
#include "constants.h"
#include "pins.h"
#include "io/KnobManager.h"
#include "audio/Synth.h"
#ifdef DINO_MODE
    #include "DinoGame.h"
#endif

Display display;

// ================================================= //
// ============= I2C Display Callback ============== //
// ================================================= //

#ifdef I2C_EXPANDER_KNOBS

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
            xSemaphoreTake(knobManager.i2cMutex, portMAX_DELAY);
            Wire.beginTransmission(u8x8_GetI2CAddress(u8x8) >> 1);
            break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            Wire.endTransmission();
            xSemaphoreGive(knobManager.i2cMutex);
            break;
    }
    return 1;
}
#endif

// ================================================= //
// ================ External state ================= //
// ================================================= //

// ================================================= //
// ================ Display private ================ //
// ================================================= //

void Display::updateState() {
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    OctaveControlMode octaveMode = sysState.octaveMode;
    xSemaphoreGive(sysState.mutex);

    uint8_t displayOctaveIdx = (octaveMode == OCTAVE_LOCAL) ? octaveIdx : octaveOffsetIdx;
    // TODO: data race — sounds[] is written by sampleISR without synchronisation.
    // Fix: read inside a critical section or maintain an ISR-safe active-notes bitmask.
    uint16_t activeNotes = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (synth.sounds[i].active) activeNotes |= (1 << synth.sounds[i].key);
    }

    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    sysState.displayState.waveform    = knobManager.knobs[waveIdx].getValue();
    sysState.displayState.volume      = knobManager.knobs[volumeIdx].getValue();
    sysState.displayState.pitch       = knobManager.knobs[pitchIdx].getValue();
    sysState.displayState.octave      = knobManager.knobs[displayOctaveIdx].getValue();
    sysState.displayState.role        = sysState.role;
    sysState.displayState.octaveMode  = sysState.octaveMode;
    sysState.displayState.activeNotes = activeNotes;
    xSemaphoreGive(sysState.mutex);
}

bool Display::stateChanged(const DisplayState &last, const DisplayState &current,
                            const std::array<uint8_t, 8> &lastMsg,
                            const std::array<uint8_t, 8> &currentMsg) {
    if (last.waveform    != current.waveform    ||
        last.volume      != current.volume       ||
        last.pitch       != current.pitch        ||
        last.octave      != current.octave       ||
        last.role        != current.role         ||
        last.octaveMode  != current.octaveMode   ||
        last.activeNotes != current.activeNotes) {
        return true;
    }
    return memcmp(lastMsg.data(), currentMsg.data(), 8) != 0;
}

// ================================================= //
// ================ Display public ================= //
// ================================================= //

void Display::update() {
    #ifdef DINO_MODE
        dinoGame.render(u8g2_);
        return;
    #endif
    updateState();

    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    std::array<uint8_t, 8> receivedMsg = sysState.RX_Message;
    std::array<uint8_t, 8> sentMsg     = sysState.TX_Message;
    SynthRole role                     = sysState.role;
    OctaveControlMode octaveMode       = sysState.octaveMode;
    DisplayState displayState          = sysState.displayState;
    xSemaphoreGive(sysState.mutex);

    std::array<uint8_t, 8> msg = {0};

    u8g2_.clearBuffer();
    u8g2_.setFont(u8g2_font_ncenB08_tr);
    u8g2_.setCursor(2, 10);

    #ifdef PROFILE_DISPLAY
        // WCET: Force the maximum number of pixels to render
        u8g2_.print("Notes: CC#DD#EFF#GG#AA#B");
        u8g2_.setCursor(2, 20);
        u8g2_.print("P: -128, W: SF, O+: 8, V: 8");
        u8g2_.setCursor(2, 30);
        u8g2_.print("R:S, P2558");
        u8g2_.sendBuffer();
    #else
        u8g2_.print("Notes: ");
        for (int i = 0; i < 12; i++) {
            if (displayState.activeNotes & (1 << i)) u8g2_.print(NOTE_NAMES[i]);
        }

        u8g2_.setCursor(2, 20);
        u8g2_.print("P: ");
        u8g2_.print(displayState.pitch);

        u8g2_.print(", W: ");
        u8g2_.print(WAVE_NAMES[displayState.waveform]);

        u8g2_.print((octaveMode == OCTAVE_OFFSET) ? ", O+:" : ", O:");
        u8g2_.print(displayState.octave);

        u8g2_.print(", V: ");
        u8g2_.print(displayState.volume);

        u8g2_.setCursor(2, 30);
        u8g2_.print("R:");
        u8g2_.print(displayState.role == SENDER ? "S" : displayState.role == RECEIVER ? "R" : "1");

        if (role != SINGLE) {
            msg = (role == SENDER) ? sentMsg : receivedMsg;
            u8g2_.print(", ");
            u8g2_.print((char)msg[0]);
            u8g2_.print(msg[1]);
            u8g2_.print(msg[2]);
            u8g2_.print(msg[3]);
            u8g2_.print(msg[4]);
            u8g2_.print(msg[5]);
        }

        if (stateChanged(lastDisplayState_, displayState, lastMsg_, msg)) {
            u8g2_.sendBuffer();
        }
    #endif

    lastDisplayState_ = displayState;
    lastMsg_ = msg;

    digitalToggle(LED_BUILTIN);
}

void Display::begin() {
    setOutMuxBit(DRST_BIT, LOW);   // Assert display logic reset
    delayMicroseconds(2);
    setOutMuxBit(DRST_BIT, HIGH);  // Release display logic reset

    #ifdef I2C_EXPANDER_KNOBS
        u8g2_.getU8x8()->byte_cb = u8x8_byte_rtos_hw_i2c;
    #endif

    u8g2_.begin();
    Wire.setClock(1000000);  // Increase I2C clock speed for faster display updates

    setOutMuxBit(DEN_BIT, HIGH);   // Enable display power supply
}

// ================================================= //
// ================ FreeRTOS Task ================== //
// ================================================= //

void displayUpdateTask(void* pvParameters) {
    #ifndef DISABLE_THREADS
        const TickType_t xFrequency = DISPLAY_INTERVAL / portTICK_PERIOD_MS;
        TickType_t xLastWakeTime = xTaskGetTickCount();
        while (1) {
            vTaskDelayUntil(&xLastWakeTime, xFrequency);
    #endif
            display.update();
    #ifndef DISABLE_THREADS
        }
    #endif
}
