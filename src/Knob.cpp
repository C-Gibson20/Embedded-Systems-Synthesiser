#include "Knob.h"

Knob::Knob(int8_t startVal, int8_t min, int8_t max) :
    rotation(startVal),
    upperLimit(max),
    lowerLimit(min),
    prevState(0b11),
    lastDirection(0),
    buttonWasPressed(false),
    buttonChanged(false),
    atomicRotation(startVal)
{
    
}

void Knob::begin() {
}

void Knob::setInitialState(uint8_t currA, uint8_t currB) {
    prevState = (currB << 1) | currA;
}

// Updates state and applies clamping
void Knob::updateRotation(uint8_t currA, uint8_t currB) {
    uint8_t currState = (currB << 1) | currA;
    int8_t change = 0;

    if (currState != prevState) {
        // "Impossible" transition logic
        if ((currState ^ prevState) == 0b11) change = lastDirection;
        
        // Normal transitions
        else if ((prevState == 0b00 && currState == 0b01) || (prevState == 0b11 && currState == 0b10)) {
            change = 1; 
            lastDirection = 1;
        } 
        else if ((prevState == 0b01 && currState == 0b00) || (prevState == 0b10 && currState == 0b11)) {
            change = -1; 
            lastDirection = -1;
        }
        prevState = currState;

        if (change != 0) {
            int8_t current = __atomic_load_n(&atomicRotation, __ATOMIC_RELAXED);
            int8_t newValue = current + change;

            if (newValue > upperLimit) newValue = upperLimit;
            if (newValue < lowerLimit) newValue = lowerLimit;

            __atomic_store_n(&atomicRotation, newValue, __ATOMIC_RELAXED);
        }
    }
}

void Knob::updateSwitch(bool bitS) {
    bool pressed = (bitS == 0);
    if (pressed && !buttonWasPressed) __atomic_store_n(&buttonChanged, true, __ATOMIC_RELEASE);
    buttonWasPressed = pressed;
}

bool Knob::isPressed() {
    return __atomic_exchange_n(&buttonChanged, false, __ATOMIC_ACQ_REL);
}

int8_t Knob::getValue() {
    return __atomic_load_n(&atomicRotation, __ATOMIC_RELAXED);
}