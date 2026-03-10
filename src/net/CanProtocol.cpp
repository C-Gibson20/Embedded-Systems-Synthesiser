#include "CanProtocol.h"
#include <ES_CAN.h>
#include "constants.h"
#include "audio/Synth.h"
#include "io/KnobManager.h"

CanProtocol canProtocol;

// ================================================= //
// ============== Encode / Decode ================== //
// ================================================= //

NoteMessage decodeMsg(const std::array<uint8_t, 8>& raw) {
    NoteMessage msg;
    msg.type     = raw[0];
    msg.keyIdx   = raw[1];
    msg.pitch    = (int8_t)raw[2];
    msg.waveform = raw[3];
    msg.octave   = raw[4];
    msg.volume   = raw[5];
    return msg;
}

void encodeNoteOn(std::array<uint8_t, 8>& out, uint8_t keyIdx, int8_t pitch, uint8_t waveform, uint8_t octave, uint8_t volume) {
    out[0] = 'P';
    out[1] = keyIdx;
    out[2] = (uint8_t)pitch;
    out[3] = waveform;
    out[4] = octave;
    out[5] = volume;
}

void encodeNoteOff(std::array<uint8_t, 8>& out, uint8_t keyIdx) {
    out[0] = 'R';
    out[1] = keyIdx;
}

// ================================================= //
// ============== CanProtocol public =============== //
// ================================================= //

void CanProtocol::begin() {
    #ifdef PROFILING_MODE
        CAN_Init(true);
    #else
        CAN_Init(false);
    #endif
    setCANFilter(0x123, 0x7ff);

    #ifndef DISABLE_ISRS
        CAN_RegisterRX_ISR(CAN_RX_ISR);
        CAN_RegisterTX_ISR(CAN_TX_ISR);
    #endif

    CAN_Start();

    msgInQ      = xQueueCreate(36, 8);
    msgOutQ     = xQueueCreate(36, 8);
    txSemaphore = xSemaphoreCreateCounting(3, 3);
}

void CanProtocol::handleKeyChange(std::bitset<32>& localInputs, std::bitset<32>& prevInputs,
                                   uint8_t keyIdx, std::array<uint8_t, 8>& TX_Message,
                                   int8_t pitch, uint8_t waveform, uint8_t octave, uint8_t volume) {
    bool isPressed  = (localInputs[keyIdx] == 0);
    bool wasPressed = (prevInputs[keyIdx] == 0);

    if (isPressed == wasPressed) return;

    if (isPressed) encodeNoteOn (TX_Message, keyIdx, pitch, waveform, octave, volume);
    else           encodeNoteOff(TX_Message, keyIdx);

    xQueueSend(msgOutQ, TX_Message.data(), 0);

    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    sysState.TX_Message = TX_Message;
    xSemaphoreGive(sysState.mutex);
}

// ================================================= //
// ============= ISR and task wrappers ============= //
// ================================================= //

void CAN_RX_ISR() {
    std::array<uint8_t, 8> RX_Message_ISR;
    uint32_t ID;
    #ifdef PROFILING_MODE
        RX_Message_ISR = {'P', 4, 1, 0, 0, 0, 0, 0};
        ID = 0x123;
        xQueueSend(canProtocol.msgInQ, RX_Message_ISR.data(), 0);
    #else
        CAN_RX(ID, RX_Message_ISR.data());
        xQueueSendFromISR(canProtocol.msgInQ, RX_Message_ISR.data(), NULL);
    #endif
}

void CAN_TX_ISR() {
    #ifdef PROFILING_MODE
        xSemaphoreGive(canProtocol.txSemaphore);
    #else
        xSemaphoreGiveFromISR(canProtocol.txSemaphore, NULL);
    #endif
}

void CAN_TX_Task(void* pvParameters) {
    #ifndef DISABLE_THREADS
        std::array<uint8_t, 8> msgOut;
        while (1) {
            xQueueReceive(canProtocol.msgOutQ, msgOut.data(), portMAX_DELAY);
            xSemaphoreTake(canProtocol.txSemaphore, portMAX_DELAY);
    #else
        std::array<uint8_t, 8> msgOut = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
    #endif
        CAN_TX(0x123, msgOut.data());
    #ifndef DISABLE_THREADS
        }
    #endif
}

void decodeTask(void* pvParameters) {
    #ifndef DISABLE_THREADS
        std::array<uint8_t, 8> localRX;
        while (1) {
            xQueueReceive(canProtocol.msgInQ, localRX.data(), portMAX_DELAY);
    #else
        std::array<uint8_t, 8> localRX = {'P', 11, 127, 5, 8, 255, 0, 0};
    #endif

        xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        SynthRole localRole = sysState.role;
        xSemaphoreGive(sysState.mutex);

        #ifdef PROFILE_DECODE
            // WCET: Force the worst-case path (RECEIVER role processing a NOTE_ON)
            uint8_t key = localRX[1];
            synth.pushNoteOn(key, localRX[5], localRX[2], (SynthWaveform)localRX[3], true,
                             std::clamp(localRX[4] + knobManager.knobs[octaveOffsetIdx].getValue(), 0, 8));
            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            sysState.RX_Message = localRX;
            xSemaphoreGive(sysState.mutex);
        #else
            if (localRole == RECEIVER) {
                NoteMessage msg = decodeMsg(localRX);
                if (msg.type == 'P') {
                    synth.pushNoteOn(msg.keyIdx, msg.volume, msg.pitch, msg.waveform, true,
                                     std::clamp(msg.octave + knobManager.knobs[octaveOffsetIdx].getValue(), 0, 8));
                } else if (msg.type == 'R') {
                    synth.pushNoteOff(msg.keyIdx, true);
                }
                xSemaphoreTake(sysState.mutex, portMAX_DELAY);
                sysState.RX_Message = localRX;
                xSemaphoreGive(sysState.mutex);
            }
        #endif

    #ifndef DISABLE_THREADS
    }
    #endif
}
