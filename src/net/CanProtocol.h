#pragma once
#include <Arduino.h>
#include <array>
#include <bitset>
#include <STM32FreeRTOS.h>
#include "SysState.h"

// ================================================= //
// ================ Message format ================= //
// ================================================= //
// byte[0]: 'P' (note-on) or 'R' (note-off)
// byte[1]: key index (0–11)
// byte[2]: pitch   (int8_t, note-on only)
// byte[3]: waveform (note-on only)
// byte[4]: octave  (note-on only)
// byte[5]: volume  (note-on only)
// byte[6–7]: unused

struct NoteMessage {
    uint8_t type;      // 'P' or 'R'
    uint8_t keyIdx;
    int8_t  pitch;
    uint8_t waveform;
    uint8_t octave;
    uint8_t volume;
};

NoteMessage decodeMsg(const std::array<uint8_t, 8>& raw);
void encodeNoteOn (std::array<uint8_t, 8>& out, uint8_t keyIdx, int8_t pitch, uint8_t waveform, uint8_t octave, uint8_t volume);
void encodeNoteOff(std::array<uint8_t, 8>& out, uint8_t keyIdx);

// ================================================= //
// ================ CanProtocol class ============== //
// ================================================= //

class CanProtocol {
public:
    SemaphoreHandle_t txSemaphore = nullptr;
    QueueHandle_t     msgInQ      = nullptr;
    QueueHandle_t     msgOutQ     = nullptr;

    void begin();

    // Detect key state change, encode, enqueue TX message, update sysState
    void handleKeyChange(std::bitset<32>& localInputs, std::bitset<32>& prevInputs,
                         uint8_t keyIdx, std::array<uint8_t, 8>& TX_Message,
                         int8_t pitch, uint8_t waveform, uint8_t octave, uint8_t volume);
};

extern CanProtocol canProtocol;

// Free functions — ISR and RTOS task entry points
void CAN_RX_ISR();
void CAN_TX_ISR();
void CAN_TX_Task(void* pvParameters);
void decodeTask(void* pvParameters);
