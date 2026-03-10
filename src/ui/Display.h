#pragma once
#include <Arduino.h>
#include "profiling.h"
#include <U8g2lib.h>
#include <STM32FreeRTOS.h>
#include <array>
#include "SysState.h"

class Display {
public:
    void begin();   // initialise hardware
    void update();  // one render cycle (call from task or profiling)

private:
    void updateState();
    bool stateChanged(const DisplayState &last, const DisplayState &current,
                      const std::array<uint8_t, 8> &lastMsg,
                      const std::array<uint8_t, 8> &currentMsg);

    U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2_{U8G2_R0};
    std::array<uint8_t, 8>                 lastMsg_          = {0};
    DisplayState                           lastDisplayState_  = {};
};

extern Display display;

// FreeRTOS task entry point — handles timing loop, delegates to display.update()
void displayUpdateTask(void* pvParameters);
