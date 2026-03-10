#pragma once
#include <Arduino.h>
#include <bitset>
#include <array>

// Result of a full 7-row matrix scan
struct KeyScanResult {
    std::bitset<32>             inputs;                // key state (bit=0 → pressed)
    bool                        westConnected = false;
    bool                        eastConnected = false;
    std::array<std::bitset<4>, 7> rowData;             // per-row column data for knob updates
};

class KeyMatrix {
public:
    // Full 7-row scan — use this in scanKeysTask
    KeyScanResult   scan();

    // Single-row scan (select + settle + read) — use during initialisation
    std::bitset<4>  scanRow(uint8_t row);

private:
    void            selectRow(uint8_t row);
    std::bitset<4>  readColumns();
};

// Output mux helper (uses the same row-select hardware)
void setOutMuxBit(uint8_t bitIdx, bool value);
