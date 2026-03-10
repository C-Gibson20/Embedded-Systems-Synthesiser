#include "KeyMatrix.h"
#include "pins.h"

// ================================================= //
// ============ Direct Port Manipulation =========== //
// ================================================= //

#if defined(ARDUINO_ARCH_STM32)
    // A5 = PA6 (REN_PIN)
    #define REN_HIGH()  (GPIOA->BSRR = (1 << 6))
    #define REN_LOW()   (GPIOA->BSRR = (1 << (6 + 16)))

    // D3 = PB0 (RA0_PIN)
    #define RA0_HIGH()  (GPIOB->BSRR = (1 << 0))
    #define RA0_LOW()   (GPIOB->BSRR = (1 << (0 + 16)))

    // D6 = PB1 (RA1_PIN)
    #define RA1_HIGH()  (GPIOB->BSRR = (1 << 1))
    #define RA1_LOW()   (GPIOB->BSRR = (1 << (1 + 16)))

    // D12 = PB4 (RA2_PIN)
    #define RA2_HIGH()  (GPIOB->BSRR = (1 << 4))
    #define RA2_LOW()   (GPIOB->BSRR = (1 << (4 + 16)))

    // D11 = PB5 (OUT_PIN)
    #define OUT_HIGH()  (GPIOB->BSRR = (1 << 5))
    #define OUT_LOW()   (GPIOB->BSRR = (1 << (5 + 16)))

    // Input masks (all Port A)
    // A2 = PA3 (C0_PIN)
    #define C0_MASK (1 << 3)
    // D9 = PA8 (C1_PIN)
    #define C1_MASK (1 << 8)
    // A6 = PA7 (C2_PIN)
    #define C2_MASK (1 << 7)
    // D1 = PA9 (C3_PIN)
    #define C3_MASK (1 << 9)
#else
    #define REN_HIGH()  (digitalWrite(REN_PIN, HIGH))
    #define REN_LOW()   (digitalWrite(REN_PIN, LOW))
    #define RA0_HIGH()  (digitalWrite(RA0_PIN, HIGH))
    #define RA0_LOW()   (digitalWrite(RA0_PIN, LOW))
    #define RA1_HIGH()  (digitalWrite(RA1_PIN, HIGH))
    #define RA1_LOW()   (digitalWrite(RA1_PIN, LOW))
    #define RA2_HIGH()  (digitalWrite(RA2_PIN, HIGH))
    #define RA2_LOW()   (digitalWrite(RA2_PIN, LOW))
    #define OUT_HIGH()  (digitalWrite(OUT_PIN, HIGH))
    #define OUT_LOW()   (digitalWrite(OUT_PIN, LOW))
#endif

// ================================================= //
// ================ KeyMatrix private ============== //
// ================================================= //

void KeyMatrix::selectRow(uint8_t row) {
    REN_LOW();

    if (row & 0x01) RA0_HIGH(); else RA0_LOW();
    if (row & 0x02) RA1_HIGH(); else RA1_LOW();
    if (row & 0x04) RA2_HIGH(); else RA2_LOW();

    #ifdef I2C_EXPANDER_KNOBS
        if (row == 2) OUT_LOW(); else OUT_HIGH();
    #else
        OUT_HIGH();
    #endif

    REN_HIGH();
    delayMicroseconds(1);
}

std::bitset<4> KeyMatrix::readColumns() {
    std::bitset<4> result;
#if defined(ARDUINO_ARCH_STM32)
    uint32_t portA = GPIOA->IDR;
    result[0] = (portA & C0_MASK) != 0;
    result[1] = (portA & C1_MASK) != 0;
    result[2] = (portA & C2_MASK) != 0;
    result[3] = (portA & C3_MASK) != 0;
#else
    result[0] = digitalRead(C0_PIN);
    result[1] = digitalRead(C1_PIN);
    result[2] = digitalRead(C2_PIN);
    result[3] = digitalRead(C3_PIN);
#endif
    return result;
}

// ================================================= //
// ================ KeyMatrix public =============== //
// ================================================= //

KeyScanResult KeyMatrix::scan() {
    KeyScanResult result;
    for (int i = 0; i < 7; i++) {
        selectRow(i);
        delayMicroseconds(3);
        std::bitset<4> cols = readColumns();

        result.rowData[i] = cols;

        int offset = i * 4;
        for (int bit = 0; bit < 4; bit++) result.inputs[offset + bit] = cols[bit];

        if (i == 5) result.westConnected = (cols[3] == 0);
        if (i == 6) result.eastConnected = (cols[3] == 0);
    }
    return result;
}

std::bitset<4> KeyMatrix::scanRow(uint8_t row) {
    selectRow(row);
    delayMicroseconds(3);
    return readColumns();
}

// ================================================= //
// ================ Output Mux Helper ============== //
// ================================================= //

void setOutMuxBit(uint8_t bitIdx, bool value) {
    REN_LOW();

    if (bitIdx & 0x01) RA0_HIGH(); else RA0_LOW();
    if (bitIdx & 0x02) RA1_HIGH(); else RA1_LOW();
    if (bitIdx & 0x04) RA2_HIGH(); else RA2_LOW();

    if (value) OUT_HIGH(); else OUT_LOW();

    REN_HIGH();
    delayMicroseconds(1);
    REN_LOW();
}
