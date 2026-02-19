#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <STM32FreeRTOS.h>
#include <Wire.h>
#include "Knob.h"

//Constants
  const uint32_t displayInterval = 100; 
  const uint32_t scanInterval = 20;
  
  //PCAL6408A Registers
  const uint8_t EXPANDER_ADDR = 0x21;
  const uint8_t REG_INPUT = 0x00;
  const uint8_t REG_PULL_EN = 0x43;
  const uint8_t REG_PULL_SEL = 0x45;
  const uint8_t REG_LAT_EN = 0x47;
  const uint8_t REG_INT_MASK = 0x4B;

  //Music Data
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
  volatile uint32_t currentStepSize = 0;

  struct {
      std::bitset<32> inputs;
      int lastPressedKey = -1;
      SemaphoreHandle_t mutex;
  } sysState;

  SemaphoreHandle_t i2cMutex; 
  SemaphoreHandle_t knobSemaphore;

  Knob knobs[4] = {
      Knob(0, 0, 8),
      Knob(0, 0, 8),
      Knob(0, 0, 8),
      Knob(2, 0, 8)  
  };
  const uint8_t volumeIdx = 3;

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
// and return the four btis as a bitset
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

void sampleISR() {
    static uint32_t phaseAcc = 0;
    uint32_t localStepSize = __atomic_load_n(&currentStepSize, __ATOMIC_RELAXED);
    int localVolumeShift = knobs[volumeIdx].getValueISR();
    
    phaseAcc += localStepSize;
    int32_t Vout = (phaseAcc >> 24) - 128;
    Vout = Vout >> (8 - localVolumeShift); 
    analogWrite(OUTR_PIN, Vout + 128);
}

void knobISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(knobSemaphore, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void knobTask(void * pvParameters) {
    uint8_t prevState = 0xFF;
    int8_t lastDirection = 0;

    while(1) {
        // Block until the expander interrupt triggers
        xSemaphoreTake(knobSemaphore, portMAX_DELAY);

        // Read Expander via I2C until the pin is released high
        do {
            xSemaphoreTake(i2cMutex, portMAX_DELAY);
            Wire.beginTransmission(EXPANDER_ADDR);
            Wire.write(REG_INPUT);
            Wire.endTransmission();
            Wire.requestFrom(EXPANDER_ADDR, (uint8_t)1);
            uint8_t currByte = Wire.read();
            xSemaphoreGive(i2cMutex);

            for (int i = 0; i < 4; i++) {
              uint8_t bitA = (currByte >> (i * 2)) & 0x01;
              uint8_t bitB = (currByte >> (i * 2 + 1)) & 0x01;
              knobs[i].update(bitA, bitB);
            }

            delayMicroseconds(10);
        } while (digitalRead(PA10) == LOW); // Loop if pin is stuck
    }
}

void scanKeysTask(void * pvParameters) {
    const TickType_t xFrequency = scanInterval/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil( &xLastWakeTime, xFrequency );

        // Key scanning loop for Rows 0-2
        std::bitset<32> localInputs;
        for (int i = 0; i < 3; i++) {
            setRow(i);
            delayMicroseconds(3);
            std::bitset<4> cols = readCols();
        
            // Map columns into 32-bit set
            int offset = i * 4;
            for (int bit = 0; bit < 4; bit++) {
                localInputs[offset + bit] = cols[bit];
            }
        }

        uint32_t localStepSize = 0;
        int localLastKey = -1;
        
        for (int i = 0; i < 12; i++) {
            // Check if the key is pressed. 
            if (localInputs[i] == 0) { 
                localStepSize = stepSizes[i];
                localLastKey = i;
            }
        }

        xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        sysState.inputs = localInputs;
        sysState.lastPressedKey = localLastKey;
        xSemaphoreGive(sysState.mutex);

        __atomic_store_n(&currentStepSize, localStepSize, __ATOMIC_RELAXED);
    }
}

void displayUpdateTask(void * pvParameters) {
    const TickType_t xFrequency = displayInterval/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
      vTaskDelayUntil( &xLastWakeTime, xFrequency );

      xSemaphoreTake(sysState.mutex, portMAX_DELAY);
      std::bitset<32> localInputs = sysState.inputs;
      int localLastKey = sysState.lastPressedKey;
      xSemaphoreGive(sysState.mutex);

      //Update display
      xSemaphoreTake(i2cMutex, portMAX_DELAY);
      u8g2.clearBuffer();                 
      u8g2.setFont(u8g2_font_ncenB08_tr); 
      u8g2.setCursor(2,10);

      // Print the state of the first 12 keys as a Hex value
      u8g2.print(localInputs.to_ulong(), HEX); 

      u8g2.setCursor(0, 20);
      u8g2.print("Note: ");
      if (localLastKey != -1) {
          u8g2.print(noteNames[localLastKey]);
      }

      u8g2.setCursor(0, 30);
      u8g2.print("Volume: "); 
      u8g2.print(knobs[volumeIdx].getValue());
      
      u8g2.sendBuffer();
      xSemaphoreGive(i2cMutex);

      //Toggle LED
      digitalToggle(LED_BUILTIN);
    }
}

void wireWrites(uint8_t enableAddress, uint8_t writeVal) {
    Wire.beginTransmission(EXPANDER_ADDR); 
    Wire.write(enableAddress); 
    Wire.write(writeVal); 
    Wire.endTransmission();
}

void clearInterruptAndSync(uint8_t address, uint8_t writeVal) {
    Wire.beginTransmission(address);
    Wire.write(writeVal);            
    Wire.endTransmission();
    Wire.requestFrom(address, (uint8_t)1);
    if (Wire.available()) {
        uint8_t startByte = Wire.read();   
        for (int i = 0; i < 4; i++) {
          uint8_t bitA = (startByte >> (i * 2)) & 0x01;
          uint8_t bitB = (startByte >> (i * 2 + 1)) & 0x01;
          knobs[i].setInitialState(bitA, bitB); // Give the class reality!
      }    
    }
}

void setup() {
  // put your setup code here, to run once:

  //Set pin directions
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

  pinMode(PA10, INPUT);

  //Initialise display
  setOutMuxBit(DRST_BIT, LOW);  //Assert display logic reset
  delayMicroseconds(2);
  setOutMuxBit(DRST_BIT, HIGH);  //Release display logic reset
  u8g2.begin();
  setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply
  setOutMuxBit(KNOB_MODE, LOW);  //Do not read knobs through key matrix

  //Initialise UART
  Serial.begin(9600);
  Serial.println("Hello World");

  // PCAL6408A Init (using i2cMutex)
  i2cMutex = xSemaphoreCreateMutex();
  sysState.mutex = xSemaphoreCreateMutex();
  knobSemaphore = xSemaphoreCreateBinary();
  Wire.begin();
  
  wireWrites(REG_PULL_EN, 0xFF);  // Enable pullups
  wireWrites(REG_LAT_EN, 0xFF);   // Enable latch
  wireWrites(REG_INT_MASK, 0x00); // Interrupt mask
  
  for (int i = 0; i < 4; i++) {
      knobs[i].begin();
  }
  clearInterruptAndSync(EXPANDER_ADDR, 0x00); 
  
  attachInterrupt(digitalPinToInterrupt(PA10), knobISR, FALLING); // To prevent 

  // Initialize the atomic variables before the timer starts
  __atomic_store_n(&currentStepSize, 0, __ATOMIC_RELAXED);

  // Initialise hardware timer
  sampleTimer.setOverflow(22000, HERTZ_FORMAT);
  sampleTimer.attachInterrupt(sampleISR);
  sampleTimer.resume();

  //Initialise and run threads
  TaskHandle_t scanKeysHandle = NULL;
  TaskHandle_t knobHandle = NULL;
  TaskHandle_t displayUpdateHandle = NULL;
  xTaskCreate(scanKeysTask, "scanKeys", 128, NULL, 2, &scanKeysHandle);
  xTaskCreate(knobTask, "knob", 128, NULL, 3, &knobHandle);
  xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);

  //Start RTOS scheduler
  vTaskStartScheduler();
}

void loop() {
  
}