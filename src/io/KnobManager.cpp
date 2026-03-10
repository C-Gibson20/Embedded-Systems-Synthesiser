#include "KnobManager.h"
#include "io/KeyMatrix.h"

// ================================================= //
// ================ Global instance ================ //
// ================================================= //

KnobManager knobManager;

// ================================================= //
// =============== KnobManager public ============== //
// ================================================= //

KnobManager::KnobManager()
    : knobs{Knob(0, -128, 127),   // PITCH
            Knob(0, 0, 5),         // WAVEFORM
            Knob(4, 0, 8),         // OCTAVE (local)
            Knob(2, 0, 8),         // VOLUME
            Knob(0, -8, 8)}        // MODE (octave offset)
{}

void KnobManager::begin(KeyMatrix& matrix) {
#ifdef I2C_EXPANDER_KNOBS
    setOutMuxBit(KNOB_MODE, LOW);   // Do not read knobs through key matrix
#else
    setOutMuxBit(KNOB_MODE, HIGH);  // Read knobs through key matrix
#endif

    for (int i = 0; i < 5; i++) knobs[i].begin();

#ifndef I2C_EXPANDER_KNOBS
    // Read initial encoder states from key matrix rows 3-4
    for (int i = 3; i < 5; i++) {
        std::bitset<4> cols = matrix.scanRow(i);
        #ifdef V1
            uint8_t knobIndex = (i == 3) ? 3 : 1;
            knobs[knobIndex].setInitialState(cols[0], cols[1]);
            knobs[knobIndex - 1].setInitialState(cols[2], cols[3]);
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

#ifdef I2C_EXPANDER_KNOBS
    initialisePCAL6408A();
#endif
}

void KnobManager::updateRotations(uint8_t rowIdx, std::bitset<4> cols, OctaveControlMode mode) {
    #ifdef V1
        if (rowIdx == 3) {
            knobs[3].updateRotation(cols[0], cols[1]);
            if (mode == OCTAVE_LOCAL) {
                knobs[2].updateRotation(cols[2], cols[3]);
                knobs[4].setInitialState(cols[2], cols[3]);
            } else {
                knobs[4].updateRotation(cols[2], cols[3]);
                knobs[2].setInitialState(cols[2], cols[3]);
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
            if (mode == OCTAVE_LOCAL) {
                knobs[2].updateRotation(cols[0], cols[1]);
                knobs[4].setInitialState(cols[0], cols[1]);
            } else {
                knobs[4].updateRotation(cols[0], cols[1]);
                knobs[2].setInitialState(cols[0], cols[1]);
            }
            knobs[1].updateRotation(cols[2], cols[3]);
        }
    #endif
}

// ================================================= //
// ========= V2 I2C expander implementation ======== //
// ================================================= //

#ifdef I2C_EXPANDER_KNOBS

void KnobManager::wireWrite(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void KnobManager::clearInterruptAndSync() {
    Wire.beginTransmission(EXPANDER_ADDR);
    Wire.write(0x00);
    Wire.endTransmission();
    Wire.requestFrom(EXPANDER_ADDR, (uint8_t)1);
    if (Wire.available()) {
        uint8_t startByte = Wire.read();
        for (int i = 0; i < 4; i++) {
            uint8_t bitA = (startByte >> (i * 2)) & 0x01;
            uint8_t bitB = (startByte >> (i * 2 + 1)) & 0x01;
            knobs[i].setInitialState(bitA, bitB);
            if (i == 2) knobs[4].setInitialState(bitA, bitB);
        }
    }
}

void KnobManager::initialisePCAL6408A() {
    i2cMutex  = xSemaphoreCreateMutex();
    semaphore = xSemaphoreCreateBinary();

    pinMode(EXPANDER_INT_PIN, INPUT_PULLUP);

    Wire.begin();
    wireWrite(REG_PULL_EN,  0xFF);  // Enable pull-ups on all pins
    wireWrite(REG_LAT_EN,   0xFF);  // Enable latched output for all pins
    wireWrite(REG_INT_MASK, 0x00);  // Enable interrupts on all pins

    clearInterruptAndSync();

    #ifndef DISABLE_ISRS
        attachInterrupt(digitalPinToInterrupt(EXPANDER_INT_PIN), knobISR, FALLING);
    #endif
}

void KnobManager::readAll(OctaveControlMode mode) {
    int retryCount = 0;
    do {
        xSemaphoreTake(i2cMutex, portMAX_DELAY);
        Wire.beginTransmission(EXPANDER_ADDR);
        Wire.write(REG_INPUT);
        Wire.endTransmission();
        Wire.requestFrom(EXPANDER_ADDR, (uint8_t)1);
        uint8_t currByte = Wire.read();
        xSemaphoreGive(i2cMutex);

        #ifdef PROFILE_KNOB
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

        uint8_t bitA = (currByte >> 4) & 0x01;
        uint8_t bitB = (currByte >> 5) & 0x01;
        if (mode == OCTAVE_LOCAL) {
            knobs[2].updateRotation(bitA, bitB);
            knobs[4].setInitialState(bitA, bitB);
        } else {
            knobs[4].updateRotation(bitA, bitB);
            knobs[2].setInitialState(bitA, bitB);
        }

        retryCount++;
        #ifndef PROFILE_KNOB
            vTaskDelay(pdMS_TO_TICKS(1));
        #endif

    #ifdef PROFILE_KNOB
        } while (retryCount < 3);
    #else
        } while (digitalRead(EXPANDER_INT_PIN) == LOW && retryCount < 3);
    #endif
}

// ================================================= //
// ============= ISR and task wrappers ============= //
// ================================================= //

void knobISR() {
    #ifdef PROFILING_MODE
        xSemaphoreGive(knobManager.semaphore);
    #else
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(knobManager.semaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    #endif
}

#endif  // I2C_EXPANDER_KNOBS

void knobTask(void* pvParameters) {
    #ifdef I2C_EXPANDER_KNOBS
        #ifndef DISABLE_THREADS
            while (1) {
                xSemaphoreTake(knobManager.semaphore, portMAX_DELAY);
        #endif
                xSemaphoreTake(sysState.mutex, portMAX_DELAY);
                OctaveControlMode mode = sysState.octaveMode;
                xSemaphoreGive(sysState.mutex);

                knobManager.readAll(mode);
        #ifndef DISABLE_THREADS
            }
        #endif
    #endif
}
