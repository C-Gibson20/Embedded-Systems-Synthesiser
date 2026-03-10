#pragma once
#include <Arduino.h>
#include "profiling.h"
#include <Wire.h>
#include <STM32FreeRTOS.h>
#include <bitset>
#include "Knob.h"
#include "SysState.h"
#include "constants.h"
#include "pins.h"
#include "KeyMatrix.h"

class KnobManager {
public:
    Knob knobs[5];

    KnobManager();

    // Full initialisation: mux mode, Knob::begin(), initial states, PCAL6408A (V2)
    void begin(KeyMatrix& matrix);

    // V2/I2C: drain one interrupt's worth of encoder data
    void readAll(OctaveControlMode mode);

    // V1/matrix: update rotations from a single scanned row
    void updateRotations(uint8_t rowIdx, std::bitset<4> cols, OctaveControlMode mode);

#ifdef I2C_EXPANDER_KNOBS
    SemaphoreHandle_t i2cMutex  = nullptr;
    SemaphoreHandle_t semaphore = nullptr;
#endif

private:
#ifdef I2C_EXPANDER_KNOBS
    static constexpr int EXPANDER_INT_PIN = PA10;
    void wireWrite(uint8_t reg, uint8_t val);
    void clearInterruptAndSync();
    void initialisePCAL6408A();
#endif
};

extern KnobManager knobManager;

// FreeRTOS task + ISR — free functions for profileTask / attachInterrupt compatibility
void knobTask(void* pvParameters);

#ifdef I2C_EXPANDER_KNOBS
void knobISR();
#endif
