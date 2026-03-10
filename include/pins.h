#pragma once
#include <Arduino.h>
// ================================================= //
// ================ Pin Definitions ================ //
// ================================================= //

//Row select and enable
constexpr int RA0_PIN = D3;
constexpr int RA1_PIN = D6;
constexpr int RA2_PIN = D12;
constexpr int REN_PIN = A5;

//Matrix input and output
constexpr int C0_PIN = A2;
constexpr int C1_PIN = D9;
constexpr int C2_PIN = A6;
constexpr int C3_PIN = D1;
constexpr int OUT_PIN = D11;

//Audio analogue out
constexpr int OUTL_PIN = A4;
constexpr int OUTR_PIN = A3;

//Joystick analogue in
constexpr int JOYY_PIN = A0;
constexpr int JOYX_PIN = A1;

//Output multiplexer bits
constexpr int KNOB_MODE = 2;
constexpr int DEN_BIT   = 3;
constexpr int DRST_BIT  = 4;
constexpr int HKOW_BIT  = 5;
constexpr int HKOE_BIT  = 6;
