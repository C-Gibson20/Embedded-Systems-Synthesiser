#pragma once

#include <Arduino.h>
#include <STM32FreeRTOS.h>
#include <algorithm> 

class Knob {
private:
    int8_t rotation;
    int8_t upperLimit;
    int8_t lowerLimit;
    uint8_t prevState;
    int8_t lastDirection;
    bool buttonWasPressed;
    bool buttonChanged;

    SemaphoreHandle_t mutex;
    volatile int8_t atomicRotation;

public:
    Knob(int8_t startVal, int8_t min, int8_t max);

    void begin();
    void setInitialState(uint8_t currA, uint8_t currB);
    void updateRotation(uint8_t currA, uint8_t currB);
    void updateSwitch(bool bitS);

    bool isPressed();

    int8_t getValue();
    int8_t getValueISR();
};
