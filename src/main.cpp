#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <STM32FreeRTOS.h>

//Constants
  const uint32_t interval = 100; //Display update interval
  
  //Formula
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

  volatile uint32_t currentStepSize = 0;

  //Tuning
  volatile int currentVolumeShift = 2;

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

// Stores system state used in more than one thread
struct {
    std::bitset<32> inputs;
    int lastPressedKey = -1;
    int knobRotation = 2;
    SemaphoreHandle_t mutex;
} sysState;

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

    // Set Row Select Enable High
    digitalWrite(REN_PIN, HIGH);
}

void sampleISR() {
    static uint32_t phaseAcc = 0;
    uint32_t localStepSize;
    int localVolumeShift;

    localStepSize = __atomic_load_n(&currentStepSize, __ATOMIC_RELAXED);
    localVolumeShift = __atomic_load_n(&currentVolumeShift, __ATOMIC_RELAXED);
    
    phaseAcc += localStepSize;

    int32_t Vout = (phaseAcc >> 24) - 128;
    Vout = Vout >> (8 - localVolumeShift); 
    analogWrite(OUTR_PIN, Vout + 128);
}

void scanKeysTask(void * pvParameters) {
    const TickType_t xFrequency = 20/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    uint8_t prevState = 0b11;
    int8_t lastDirection = 0;

    while (1) {
        vTaskDelayUntil( &xLastWakeTime, xFrequency );

        std::bitset<32> localInputs;
        int localRotationChange = 0;

        // Key scanning loop for Rows 0-3
        for (int i = 0; i < 4; i++) {
            setRow(i);
            delayMicroseconds(3);

            std::bitset<4> cols = readCols();
            
            // Map columns into 32-bit set
            int offset = i * 4;
            for (int bit = 0; bit < 4; bit++) {
                localInputs[offset + bit] = cols[bit];
            }
        }

        // Decode Knob 3
        uint8_t currA = localInputs[12];
        uint8_t currB = localInputs[13];
        uint8_t currState = (currB << 1) | currA;

        // Apply state transation table logic
        if (prevState != currState) {
            if ((currState ^ prevState) == 0b11) {
                localRotationChange = lastDirection;
            }
            else if ((prevState == 0b00 && currState == 0b01) || (prevState == 0b11 && currState == 0b10)) {
                localRotationChange = 1;
                lastDirection = 1;
            } 
            else if ((prevState == 0b01 && currState == 0b00) || (prevState == 0b10 && currState == 0b11)) {
                localRotationChange = -1;
                lastDirection = -1;
            }
            prevState = currState;
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
        
        int rawNewValue = sysState.knobRotation + localRotationChange;
        sysState.knobRotation = std::clamp(rawNewValue, 0, 8);
        int localKnob = sysState.knobRotation;
        xSemaphoreGive(sysState.mutex);

        __atomic_store_n(&currentStepSize, localStepSize, __ATOMIC_RELAXED);
        __atomic_store_n(&currentVolumeShift, localKnob, __ATOMIC_RELAXED);
    }
}

void displayUpdateTask(void * pvParameters) {
    const TickType_t xFrequency = 100/portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
      vTaskDelayUntil( &xLastWakeTime, xFrequency );

      //Update display
      u8g2.clearBuffer();                 
      u8g2.setFont(u8g2_font_ncenB08_tr); 
      u8g2.setCursor(2,10);

      // Print the state of the first 12 keys as a Hex value
      xSemaphoreTake(sysState.mutex, portMAX_DELAY);
      std::bitset<32> localInputs = sysState.inputs;
      int localLastKey = sysState.lastPressedKey;
      int localKnob = sysState.knobRotation;
      xSemaphoreGive(sysState.mutex);

      u8g2.print(localInputs.to_ulong(), HEX); 

      u8g2.setCursor(0, 20);
      u8g2.print("Note: ");
      if (localLastKey != -1) {
          u8g2.print(noteNames[localLastKey]);
      }

      u8g2.setCursor(0, 30);
      u8g2.print("K:"); 
      u8g2.print(localKnob);
      
      u8g2.sendBuffer();

      //Toggle LED
      digitalToggle(LED_BUILTIN);
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

  //Initialise display
  setOutMuxBit(DRST_BIT, LOW);  //Assert display logic reset
  delayMicroseconds(2);
  setOutMuxBit(DRST_BIT, HIGH);  //Release display logic reset
  u8g2.begin();
  setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply
  setOutMuxBit(KNOB_MODE, HIGH);  //Read knobs through key matrix

  //Initialise UART
  Serial.begin(9600);
  Serial.println("Hello World");

  // Initialize the atomic variables before the timer starts
  __atomic_store_n(&currentVolumeShift, sysState.knobRotation, __ATOMIC_RELAXED);
  __atomic_store_n(&currentStepSize, 0, __ATOMIC_RELAXED);

  // Initialise hardware timer
  sampleTimer.setOverflow(22000, HERTZ_FORMAT);
  sampleTimer.attachInterrupt(sampleISR);
  sampleTimer.resume();

  //Initialise and run thread
  TaskHandle_t scanKeysHandle = NULL;
  xTaskCreate(
    scanKeysTask,
    "scanKeys",
    64,
    NULL,
    2,
    &scanKeysHandle
  );

  TaskHandle_t displayUpdateHandle = NULL;
  xTaskCreate(
    displayUpdateTask,
    "displayUpdate",
    256,
    NULL,
    1,
    &displayUpdateHandle
  );

  //Create mutex and assign handle
  sysState.mutex = xSemaphoreCreateMutex();

  //Start RTOS scheduler
  vTaskStartScheduler();
}

void loop() {
  
}