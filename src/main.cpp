#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <STM32FreeRTOS.h>
#include <Wire.h>
#include "Knob.h"
#include <ES_CAN.h>

/* --- PROFILING SYSTEM --- */
// #define PROFILING_MODE           // Disables scheduler and ISRs globally

#ifdef PROFILING_MODE
  // #define DISABLE_THREADS
  // #define DISABLE_ISRS
  
  // #define PROFILE_SCANKEYS
  // #define PROFILE_DISPLAY
  // #define PROFILE_DECODE
  // #define PROFILE_KNOB
  // #define PROFILE_CAN_TX

  // #define PROFILE_SAMPLE_ISR
  // #define PROFILE_CAN_RX_ISR
  // #define PROFILE_CAN_TX_ISR
  // #define PROFILE_KNOB_ISR
#endif
/* --------------------------- */

//Constants
  const uint32_t displayInterval = 100; 
  const uint32_t scanInterval = 20;
  
  //PCAL6408A Registers
  const uint8_t EXPANDER_ADDR = 0x21;
  const uint8_t REG_INPUT = 0x00;
  const uint8_t REG_PULL_EN = 0x43;
  const uint8_t REG_PULL_SEL = 0x44; 
  const uint8_t REG_LAT_EN = 0x42;   
  const uint8_t REG_INT_MASK = 0x45; 

  //Music Data
  const int octave = 4;
  const double fs = 22000.0;
  const double pow2_32 = 4294967296.0;

  const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

  constexpr float f_notes[] = {
    261.63f, 277.18f, 293.66f, 311.13f, // C, C#, D, D#
    329.63f, 349.23f, 369.99f, 392.00f, // E, F, F#, G
    415.30f, 440.00f, 466.16f, 493.88f  // G#, A, A#, B
  };

  constexpr uint32_t stepSizes[] = {
      (uint32_t)(f_notes[0] * pow2_32 / fs),
      (uint32_t)(f_notes[1] * pow2_32 / fs),
      (uint32_t)(f_notes[2] * pow2_32 / fs),
      (uint32_t)(f_notes[3] * pow2_32 / fs),
      (uint32_t)(f_notes[4] * pow2_32 / fs),
      (uint32_t)(f_notes[5] * pow2_32 / fs),
      (uint32_t)(f_notes[6] * pow2_32 / fs),
      (uint32_t)(f_notes[7] * pow2_32 / fs),
      (uint32_t)(f_notes[8] * pow2_32 / fs),
      (uint32_t)(f_notes[9] * pow2_32 / fs),
      (uint32_t)(f_notes[10] * pow2_32 / fs),
      (uint32_t)(f_notes[11] * pow2_32 / fs)
  };

//Shared state
enum SynthRole { SENDER, RECEIVER };

volatile uint32_t currentStepSize = 0;

struct {
    SynthRole role = RECEIVER;
    std::bitset<32> inputs;
    int lastPressedKey = -1;
    std::array<uint8_t, 8> RX_Message = {0};
    SemaphoreHandle_t mutex;
} sysState;

// SemaphoreHandle_t knobSemaphore;
SemaphoreHandle_t CAN_TX_Semaphore;

Knob knobs[4] = {
    Knob(0, 0, 8),
    Knob(0, 0, 8),
    Knob(4, 0, 8),
    Knob(2, 0, 8)  
};
const uint8_t volumeIdx = 3;
const uint8_t octaveIdx = 2;

//CAN Bus Communication
QueueHandle_t msgInQ;
QueueHandle_t msgOutQ;

//Pin definitions
  //Row select and enable
  const int RA0_PIN = D3;
  const int RA1_PIN = D6;
  const int RA2_PIN = D12;
  const int REN_PIN = A5;

  //Matrix input and output
  const int C0_PIN = A2;
  const int C1_PIN = D9;
  const int C2_PIN = A6;
  const int C3_PIN = D1;
  const int OUT_PIN = D11;

  //Audio analogue out
  const int OUTL_PIN = A4;
  const int OUTR_PIN = A3;

  //Joystick analogue in
  const int JOYY_PIN = A0;
  const int JOYX_PIN = A1;

  //Output multiplexer bits
  const int KNOB_MODE = 2;
  const int DEN_BIT = 3;
  const int DRST_BIT = 4;
  const int HKOW_BIT = 5;
  const int HKOE_BIT = 6;

//Display driver object
U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

//Hardware Timer
HardwareTimer sampleTimer(TIM1);

// ================================================= //
// ================ Hardware Helpers =============== //
// ================================================= //

//Function to set outputs using key matrix
void setOutMuxBit(const uint8_t bitIdx, const bool value) {
      digitalWrite(REN_PIN,LOW);
      digitalWrite(RA0_PIN, bitIdx & 0x01);
      digitalWrite(RA1_PIN, bitIdx & 0x02);
      digitalWrite(RA2_PIN, bitIdx & 0x04);
      digitalWrite(OUT_PIN,value);
      digitalWrite(REN_PIN,HIGH);
      delayMicroseconds(2);
      digitalWrite(REN_PIN,LOW);
}

// Function to read the inputs from the four columns of the switch matrix
std::bitset<4> readCols() {
    std::bitset<4> result;

    // Read the columns
    result[0] = digitalRead(C0_PIN);
    result[1] = digitalRead(C1_PIN);
    result[2] = digitalRead(C2_PIN);
    result[3] = digitalRead(C3_PIN);

    return result;
}

void setRow(uint8_t rowIdx){
    // Set Row Select Enable low
    digitalWrite(REN_PIN, LOW);

    // Set Row Select Address low
    digitalWrite(RA0_PIN, rowIdx & 0x01);
    digitalWrite(RA1_PIN, rowIdx & 0x02);
    digitalWrite(RA2_PIN, rowIdx & 0x04);

    // Latch for KNOB_MODE
    digitalWrite(OUT_PIN, (rowIdx == 2) ? LOW : HIGH);

    // Set Row Select Enable High
    digitalWrite(REN_PIN, HIGH);
    delayMicroseconds(2);
}

// ================================================= //
// ============= Interrupt Subroutines ============= //
// ================================================= //

void sampleISR() {
    static uint32_t phaseAcc = 0;
    uint32_t localStepSize = __atomic_load_n(&currentStepSize, __ATOMIC_RELAXED);
    int localVolumeShift = knobs[volumeIdx].getValueISR();
    
    phaseAcc += localStepSize;
    int32_t Vout = (phaseAcc >> 24) - 128;
    Vout = Vout >> (8 - localVolumeShift); 
    analogWrite(OUTR_PIN, Vout + 128);
}

// void knobISR() {
//   #ifdef PROFILING_MODE
//   xSemaphoreGive(knobSemaphore);
//   #else
//   BaseType_t xHigherPriorityTaskWoken = pdFALSE;
//   xSemaphoreGiveFromISR(knobSemaphore, &xHigherPriorityTaskWoken);
//   portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
//   #endif
// }

void CAN_RX_ISR (void) {
    std::array<uint8_t, 8> RX_Message_ISR;
    uint32_t ID;
    #ifdef PROFILING_MODE
    RX_Message_ISR = {'P', 4, 1, 0, 0, 0, 0, 0}; 
    ID = 0x123;
    xQueueSend(msgInQ, RX_Message_ISR.data(), 0);
    #else
    CAN_RX(ID, RX_Message_ISR.data());
    xQueueSendFromISR(msgInQ, RX_Message_ISR.data(), NULL);
    #endif
}

void CAN_TX_ISR (void) {
	  #ifdef PROFILING_MODE
    xSemaphoreGive(CAN_TX_Semaphore);
    #else
    xSemaphoreGiveFromISR(CAN_TX_Semaphore, NULL);
    #endif
}

// ================================================= //
// ================== Task Helpers ================= //
// ================================================= //

void handleSynthRole(bool westConnected, bool eastConnected) {
    if (westConnected || eastConnected) {
        if (!westConnected && eastConnected) {
          sysState.role = SENDER;
        } else {
          sysState.role = RECEIVER;
        }
    }
    else if (knobs[octaveIdx].isPressed()) {
        sysState.role = (sysState.role == RECEIVER) ? SENDER : RECEIVER;
    }

    if (sysState.role == SENDER) {
        __atomic_store_n(&currentStepSize, 0, __ATOMIC_RELAXED); // Silent Sender
    }
}

void updateSwitchesAndConnections(std::bitset<4> cols, uint8_t rowIdx, bool &westConnected, bool &eastConnected) {
    if (rowIdx == 5) {
        westConnected = (cols[3] == 0);
        knobs[0].updateSwitch(cols[0]); // C0: Knob 0 S
        knobs[3].updateSwitch(cols[1]); // C1: Knob 3 S
    } else if (rowIdx == 6) {
        eastConnected = (cols[3] == 0);
        knobs[1].updateSwitch(cols[0]); // C0: Knob 1 S
        knobs[2].updateSwitch(cols[1]); // C1: Knob 2 S
    }
}

void mapColumnsToSet(std::bitset<32> &localInputs, std::bitset<4> cols, uint8_t rowIdx) {
    int offset = rowIdx * 4;
    for (int bit = 0; bit < 4; bit++) {
        localInputs[offset + bit] = cols[bit];
    }
}

void constructAndSendTXMessage(
  std::bitset<32> localInputs, 
  std::bitset<32> prevInputs, 
  uint8_t keyIdx, 
  std::array<uint8_t, 8> &TX_Message,
  uint32_t &localStepSize,
  int &localLastKey
) {
    bool isPressed = (localInputs[keyIdx] == 0);
    bool wasPressed = (prevInputs[keyIdx] == 0);
    int octave = knobs[octaveIdx].getValue();

    if (isPressed) {
        localStepSize = stepSizes[keyIdx];
        int8_t shift = octave - 4;
        if (shift > 0) localStepSize <<= shift; 
        else if (shift < 0) localStepSize >>= abs(shift);
        localLastKey = keyIdx;
    }

    if (isPressed != wasPressed) {
        TX_Message[0] = isPressed ? 'P' : 'R';
        TX_Message[1] = octave;
        TX_Message[2] = keyIdx;
        xQueueSend(msgOutQ, TX_Message.data(), portMAX_DELAY);
    }
}

// ================================================= //
// ===================== Tasks ===================== //
// ================================================= //

void scanKeysTask(void * pvParameters) {
    const TickType_t xFrequency = scanInterval/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static std::bitset<32> prevInputs;
    static std::array<uint8_t, 8> TX_Message = {0};
    static uint32_t lastStepSize = 0;
    static bool westConnected = false;
    static bool eastConnected = false;

    #ifndef DISABLE_THREADS
    while (1) {
        vTaskDelayUntil( &xLastWakeTime, xFrequency );
    #endif

        // Key scanning loop for Rows 0-2
        std::bitset<32> localInputs;
        for (int i = 0; i < 7; i++) { 
            
            setRow(i);
            delayMicroseconds(3);
            std::bitset<4> cols = readCols();

            if (3 <= i  && i < 5) {
                uint8_t knobIndex = (i == 3) ? 3 : 1;
                knobs[knobIndex].updateRotation(cols[0],cols[1]);
                knobs[knobIndex-1].updateRotation(cols[2],cols[3]);
            }
            // Map rows 5 and 6 to knob switches
            // and updates east and west connections
            updateSwitchesAndConnections(cols, i, westConnected, eastConnected);

            // Map columns into 32-bit set
            mapColumnsToSet(localInputs, cols, i);
        }

        uint32_t localStepSize = 0;
        int localLastKey = -1;

        #ifdef PROFILE_SCANKEYS
            // WCET: Force 12 messages to be sent every time regardless of actual state
            for (int i = 0; i < 12; i++) {
                TX_Message[0] = 'P'; // Force "Pressed" status
                TX_Message[1] = 4;   // Fixed octave
                TX_Message[2] = i;   // Key index
                xQueueSend(msgOutQ, TX_Message.data(), 0); // Non-blocking send
            }
        #else
        // for (int i = 0; i < 4; i++) {
        //     uint8_t bitA = localInputs[6 + (i * 2)];
        //     uint8_t bitB = localInputs[6 + (i * 2) + 1];
        //     knobs[i].updateRotation(bitA, bitB);
        // }
        
        for (int i = 0; i < 12; i++) {
            constructAndSendTXMessage(localInputs, prevInputs, i, TX_Message, localStepSize, localLastKey);
        }
        #endif
        prevInputs = localInputs;

        xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        sysState.inputs = localInputs;
        sysState.lastPressedKey = localLastKey;
        handleSynthRole(westConnected, eastConnected);
        xSemaphoreGive(sysState.mutex);

        // Only the receiver updates the local sound
        if ((sysState.role == RECEIVER) && (localStepSize != lastStepSize)) {
            __atomic_store_n(&currentStepSize, localStepSize, __ATOMIC_RELAXED);
            lastStepSize = localStepSize;
        } 
    #ifndef DISABLE_THREADS
    }
    #endif
}

void decodeTask(void * pvParameters) {
  #ifndef DISABLE_THREADS
  std::array<uint8_t, 8> localRX;
  
  while (1) {
      // Block until message available in queue
      xQueueReceive(msgInQ, localRX.data(), portMAX_DELAY);
  #else
  // Initialize with a worst-case index (e.g., key 11)
  std::array<uint8_t, 8> localRX = {'P', 4, 11, 0, 0, 0, 0, 0};
  #endif

      xSemaphoreTake(sysState.mutex, portMAX_DELAY);
      SynthRole localRole = sysState.role;
      xSemaphoreGive(sysState.mutex);

      #ifdef PROFILE_DECODE
      int8_t senderOctave = localRX[1];
      uint32_t localStepSize = stepSizes[localRX[2]];
      localStepSize <<= 1; // Force a shift operation

      __atomic_store_n(&currentStepSize, localStepSize, __ATOMIC_RELAXED);
      #else
      if (localRole == RECEIVER) {
          int8_t senderOctave = localRX[1];
          uint32_t localStepSize = (localRX[0] == 'P') ? stepSizes[localRX[2]] : 0;
          int8_t shift = senderOctave - 4;
          if (shift > 0) localStepSize <<= shift; 
          else if (shift < 0) localStepSize >>= abs(shift);

          __atomic_store_n(&currentStepSize, localStepSize, __ATOMIC_RELAXED);

          xSemaphoreTake(sysState.mutex, portMAX_DELAY);
          sysState.RX_Message = localRX;
          xSemaphoreGive(sysState.mutex);
      }
      #endif
  #ifndef DISABLE_THREADS
  }
  #endif
}

void CAN_TX_Task (void * pvParameters) {
  #ifndef DISABLE_THREADS
	std::array<uint8_t, 8> msgOut;
	while (1) {
		xQueueReceive(msgOutQ, msgOut.data(), portMAX_DELAY);
		xSemaphoreTake(CAN_TX_Semaphore, portMAX_DELAY);
  #else
  std::array<uint8_t, 8> msgOut = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
  #endif
		CAN_TX(0x123, msgOut.data());
  #ifndef DISABLE_THREADS
	}
  #endif
}

void displayUpdateTask(void * pvParameters) {
    const TickType_t xFrequency = displayInterval/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    #ifndef DISABLE_THREADS
    while (1) { // Standard RTOS mode
      vTaskDelayUntil( &xLastWakeTime, xFrequency );
    #endif
    

      xSemaphoreTake(sysState.mutex, portMAX_DELAY);
      std::bitset<32> localInputs = sysState.inputs;
      int localLastKey = sysState.lastPressedKey;
      std::array<uint8_t, 8> localMsg = sysState.RX_Message;
      SynthRole role = sysState.role;
      xSemaphoreGive(sysState.mutex);
      
      //Update display
      u8g2.clearBuffer();                 
      u8g2.setFont(u8g2_font_ncenB08_tr); 
      u8g2.setCursor(2,10);

      #ifdef PROFILE_DISPLAY
      u8g2.print("FFFFFFFF Note: G#"); // Max length string
      u8g2.setCursor(2, 20);
      u8g2.print("K: 8, 8, Oct: 8, Vol: 8"); // Max value strings
      u8g2.setCursor(2,30);
      u8g2.print("FFF"); // Max length string
      u8g2.setCursor(50,30);
      u8g2.print("Role: SENDER");
      #else
      // Print the state of the first 12 keys as a Hex value
      u8g2.print(localInputs.to_ulong(), HEX); 
      u8g2.print("  Note: ");
      if (localLastKey != -1) {
          u8g2.print(noteNames[localLastKey]);
      }
      u8g2.setCursor(2, 20);
      u8g2.print("K: "); 
      for (int i = 0; i < 2; i++) {
          u8g2.print(knobs[i].getValue());
          u8g2.print(", "); 
      }
      u8g2.print("Oct: "); 
      u8g2.print(knobs[octaveIdx].getValue());
      u8g2.print(", Vol: "); 
      u8g2.print(knobs[volumeIdx].getValue());
      u8g2.setCursor(2,30);
      u8g2.print((char) localMsg[0]);
      u8g2.print(localMsg[1]);
      u8g2.print(localMsg[2]);
      u8g2.setCursor(50,30);
      u8g2.print("Role: ");
      u8g2.print((role == SENDER) ? "S": "R");
      #endif
      
      u8g2.sendBuffer();

      //Toggle LED
      digitalToggle(LED_BUILTIN);
    #ifndef DISABLE_THREADS
    }
    #endif
}

// ================================================= //
// ================= Setup Helpers ================= //
// ================================================= //

void wireWrites(uint8_t enableAddress, uint8_t writeVal) {
    Wire.beginTransmission(EXPANDER_ADDR); 
    Wire.write(enableAddress); 
    Wire.write(writeVal); 
    Wire.endTransmission();
}

void clearInterruptAndSync(uint8_t address, uint8_t writeVal) {

    for (int i = 0; i < 4; i++) {
        uint8_t bitA = 0b1;
        uint8_t bitB = 0b1;
        knobs[i].setInitialState(bitA, bitB); 
    }  
}

void setPinDirections() {
    pinMode(RA0_PIN, OUTPUT);
    pinMode(RA1_PIN, OUTPUT);
    pinMode(RA2_PIN, OUTPUT);
    pinMode(REN_PIN, OUTPUT);
    pinMode(OUT_PIN, OUTPUT);
    pinMode(OUTL_PIN, OUTPUT);
    pinMode(OUTR_PIN, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);

    pinMode(C0_PIN, INPUT);
    pinMode(C1_PIN, INPUT);
    pinMode(C2_PIN, INPUT);
    pinMode(C3_PIN, INPUT);
    pinMode(JOYX_PIN, INPUT);
    pinMode(JOYY_PIN, INPUT);

    // pinMode(PA10, INPUT_PULLUP);
}

void initialiseDisplay() {
    setOutMuxBit(DRST_BIT, LOW);  //Assert display logic reset
    delayMicroseconds(2);
    setOutMuxBit(DRST_BIT, HIGH);  //Release display logic reset
    u8g2.begin();
    setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply
    setOutMuxBit(KNOB_MODE, HIGH);  //Do read knobs through key matrix
}

void initialiseCANBus() {
    CAN_Init(false);
    setCANFilter(0x123,0x7ff);

    #ifndef DISABLE_ISRS
    CAN_RegisterRX_ISR(CAN_RX_ISR);
    CAN_RegisterTX_ISR(CAN_TX_ISR);
    #endif
    
    CAN_Start();

    msgInQ = xQueueCreate(36, 8);
    msgOutQ = xQueueCreate(384, 8); // Increased to hold 32 iterations of 12 key messages

    CAN_TX_Semaphore = xSemaphoreCreateCounting(3,3);
}

void initialisePCAL6408A() {
    sysState.mutex = xSemaphoreCreateMutex();
    
    for (int i = 0; i < 4; i++) {
        knobs[i].begin();
    }
    clearInterruptAndSync(EXPANDER_ADDR, 0x00); 
    
    #ifndef DISABLE_ISRS
    #endif
}

void initialiseHardwareTimer() {
    sampleTimer.setOverflow(22000, HERTZ_FORMAT);

    #ifndef DISABLE_ISRS
    sampleTimer.attachInterrupt(sampleISR);
    #endif

    sampleTimer.resume();
}

void initialiseThreads() {
    #ifndef DISABLE_THREADS
    TaskHandle_t scanKeysHandle = NULL;
    TaskHandle_t decodeHandle = NULL;
    TaskHandle_t displayUpdateHandle = NULL;
    TaskHandle_t canTxHandle = NULL;
    xTaskCreate(scanKeysTask, "scanKeys", 128, NULL, 3, &scanKeysHandle);
    xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);
    xTaskCreate(decodeTask, "decode", 128, NULL, 2, &decodeHandle);
    xTaskCreate(CAN_TX_Task, "canTX", 128, NULL, 2, &canTxHandle);
    #endif
}

// ================================================= //
// ===================== Setup ===================== //
// ================================================= //

void setup() {
  // put your setup code here, to run once:

  //Set pin directions
  setPinDirections();

  //Initialise display
  initialiseDisplay();

  //Initialise UART
  Serial.begin(9600);
  Serial.println("Hello World");

  //Initialise CAN bus
  initialiseCANBus();

  //Initialise PCAL6408A (using i2cMutex)
  initialisePCAL6408A();

  // Initialize the atomic variables before the timer starts
  __atomic_store_n(&currentStepSize, 0, __ATOMIC_RELAXED);

  // Initialise hardware timer
  initialiseHardwareTimer();

  //Initialise and run threads
  initialiseThreads();

  #ifndef DISABLE_THREADS
  //Start RTOS scheduler
  vTaskStartScheduler();
  #endif
}

// ================================================= //
// ===================== Loop ====================== //
// ================================================= //

void loop() {
    #ifdef PROFILING_MODE
    uint32_t startTime = 0;
    uint32_t endTime = 0;
    const int iterations = 32;

    #ifdef PROFILE_SCANKEYS
    xQueueReset(msgOutQ);

    startTime = micros();

    for(int i = 0; i < iterations; i++){
      scanKeysTask(NULL);
    }

    endTime = micros();
    Serial.print("scanKeys ");
    
    #endif

    #ifdef PROFILE_DISPLAY
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
      displayUpdateTask(NULL);
    }
    endTime = micros();
    Serial.print("DisplayUpdate ");
    #endif

    #ifdef PROFILE_DECODE
    // Pre-fill the queue so decodeTask has something to "process" even if scheduler is off
    uint8_t dummyMsg[8] = {'P', 4, 1, 0, 0, 0, 0, 0};
    for(int i = 0; i < iterations; i++) {
      xQueueSend(msgInQ, dummyMsg, 0);
    }
    
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
      decodeTask(NULL);
    }
    endTime = micros();
    Serial.print("DecodeTask ");
    #endif

    #ifdef PROFILE_KNOB
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
      knobTask(NULL);
    }
    endTime = micros();
    Serial.print("KnobTask ");
    #endif

    #ifdef PROFILE_CAN_TX
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
      CAN_TX_Task(NULL);
    }
    endTime = micros();
    Serial.print("CAN_TX_Task ");
    #endif

    #ifdef PROFILE_SAMPLE_ISR
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
        sampleISR();
    }
    endTime = micros();
    Serial.print("SampleISR ");
    #endif

    #ifdef PROFILE_CAN_RX_ISR
    xQueueReset(msgInQ);
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
        CAN_RX_ISR();
    }
    endTime = micros();
    Serial.print("CAN_RX_ISR ");
    #endif

    #ifdef PROFILE_CAN_TX_ISR
    vSemaphoreDelete(CAN_TX_Semaphore);
    CAN_TX_Semaphore = xSemaphoreCreateCounting(255, 0);
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
        CAN_TX_ISR();
    }
    endTime = micros();
    Serial.print("CAN_TX_ISR ");
    #endif

    #ifdef PROFILE_KNOB_ISR
    xSemaphoreTake(knobSemaphore, 0);
    startTime = micros();
    for(int i = 0; i < iterations; i++) {
        knobISR();
    }
    endTime = micros();
    Serial.print("KnobISR ");
    #endif

    float totalTime = endTime - startTime;
    Serial.print("Average WCET: ");
    Serial.print(totalTime / iterations);
    Serial.println(" us");

    while(1); // Stop execution
    #endif
}