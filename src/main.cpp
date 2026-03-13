#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <HardwareTimer.h>
#include <STM32FreeRTOS.h>
#include <ES_CAN.h>

//Variables in this code use atomic access

//#define MODE_BOTH
#define MODE_SENDER
//#define MODE_RECEIVER

//Stack Sizes
//#define StackSizeScanKeys
//#define StackSizeDisplayUpdate
//#define StackSizeDecode
//#define StackSizeCAN_TX

//Measuring worst case execution time
//#define TEST_SCANKEYS
//#define TEST_DISPLAY
//#define TEST_DECODE
//#define TEST_CANTX
//#define TEST_SAMPLE_ISR

#if defined(TEST_SCANKEYS) || defined(TEST_DISPLAY) || defined(TEST_DECODE) || defined(TEST_CANTX) || defined(TEST_SAMPLE_ISR)
  #define DISABLE_THREADS
#endif

// ===== Timing / IDs =====
constexpr uint32_t AUDIO_FS_HZ        = 22000;
constexpr uint32_t SCAN_PERIOD_MS     = 20;
constexpr uint32_t DISPLAY_PERIOD_MS  = 100;
constexpr uint32_t CAN_ID             = 0x123;
constexpr uint8_t  FIXED_OCTAVE       = 5;

// ===== Pins =====
const int RA0_PIN = D3;
const int RA1_PIN = D6;
const int RA2_PIN = D12;
const int REN_PIN = A5;

const int C0_PIN  = A2;
const int C1_PIN  = D9;
const int C2_PIN  = A6;
const int C3_PIN  = D1;
const int OUT_PIN = D11;

const int OUTL_PIN = A4;
const int OUTR_PIN = A3;

const int JOYY_PIN = A0;
const int JOYX_PIN = A1;

const int KNOB_MODE = 2;
const int DEN_BIT   = 3;
const int DRST_BIT  = 4;
const int HKOW_BIT  = 5;
const int HKOE_BIT  = 6;

U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

// ===== Knob =====
class Knob {
private:
  volatile int8_t Rotation;
  int8_t minLimit;
  int8_t MaxLimit;
  uint8_t prevState;
  int8_t lastDirection;

public:
  Knob(int8_t min = 0, int8_t max = 8, int8_t initial = 0)
    : Rotation(initial), minLimit(min), MaxLimit(max), prevState(0b11), lastDirection(0) {}

  void update(bool A, bool B) {
    uint8_t currentState = (B << 1) | A;
    int8_t change = 0;

    if      (prevState == 0b00 && currentState == 0b01) { change =  1; lastDirection =  1; }
    else if (prevState == 0b01 && currentState == 0b00) { change = -1; lastDirection = -1; }
    else if (prevState == 0b10 && currentState == 0b11) { change = -1; lastDirection = -1; }
    else if (prevState == 0b11 && currentState == 0b10) { change =  1; lastDirection =  1; }
    else if (prevState == 0b00 && currentState == 0b11) change = lastDirection;
    else if (prevState == 0b01 && currentState == 0b10) change = lastDirection;
    else if (prevState == 0b10 && currentState == 0b01) change = lastDirection;
    else if (prevState == 0b11 && currentState == 0b00) change = lastDirection;

    prevState = currentState;

    int8_t newRotation = Rotation + change;
    if (newRotation > MaxLimit) newRotation = MaxLimit;
    if (newRotation < minLimit) newRotation = minLimit;

    __atomic_store_n(&Rotation, newRotation, __ATOMIC_RELAXED);
  }

  int8_t read() const {
    return __atomic_load_n(&Rotation, __ATOMIC_RELAXED);
  }
};

Knob knob0(0, 8);
Knob knob1(0, 8);
Knob knob2(0, 8);
Knob knob3(0, 8);

// ===== System State =====
struct {
  std::bitset<32> inputs;
  volatile uint8_t knob3Rotation;
  uint8_t rxMsg[8];
  SemaphoreHandle_t mutex;
} sysState;

// ===== Audio State =====
struct {
  volatile uint16_t localMask;
  volatile uint16_t remoteMask;
  volatile uint32_t remoteFreqs[12];
} audioState = {0, 0, {0}};

HardwareTimer sampleTimer(TIM1);

const uint32_t stepSizes[12] = {
  51076057, 54113197, 57330935, 60740010,
  64351799, 68178356, 72232452, 76527617,
  81078186, 85899346, 91007187, 96418756
};

// msgOutQ now carries one bitmask message per scan, not per key event
// Queue size of 4 is plenty since scanKeys sends at most 1 per 20ms
QueueHandle_t msgInQ  = nullptr;
QueueHandle_t msgOutQ = nullptr;

SemaphoreHandle_t CAN_TX_Semaphore;

// ===== Mutex-protected sysState accessors =====

void updateInputs(const std::bitset<32>& newInputs) {
  xSemaphoreTake(sysState.mutex, portMAX_DELAY);
  sysState.inputs = newInputs;
  xSemaphoreGive(sysState.mutex);
}

std::bitset<32> getInputs() {
  xSemaphoreTake(sysState.mutex, portMAX_DELAY);
  std::bitset<32> copy = sysState.inputs;
  xSemaphoreGive(sysState.mutex);
  return copy;
}

void updateRxMsg(const uint8_t msg[8]) {
  xSemaphoreTake(sysState.mutex, portMAX_DELAY);
  for (int i = 0; i < 8; i++) sysState.rxMsg[i] = msg[i];
  xSemaphoreGive(sysState.mutex);
}

void getRxMsg(uint8_t msg[8]) {
  xSemaphoreTake(sysState.mutex, portMAX_DELAY);
  for (int i = 0; i < 8; i++) msg[i] = sysState.rxMsg[i];
  xSemaphoreGive(sysState.mutex);
}

void getInputsAndRxMsg(std::bitset<32>& inputs, uint8_t msg[8]) {
  xSemaphoreTake(sysState.mutex, portMAX_DELAY);
  inputs = sysState.inputs;
  for (int i = 0; i < 8; i++) msg[i] = sysState.rxMsg[i];
  xSemaphoreGive(sysState.mutex);
}

// ===== Atomic audioState accessors =====

void setLocalMask(uint16_t mask) {
  __atomic_store_n(&audioState.localMask, mask, __ATOMIC_RELAXED);
}

uint16_t getLocalMask() {
  return __atomic_load_n(&audioState.localMask, __ATOMIC_RELAXED);
}

void setRemoteMask(uint16_t mask) {
  __atomic_store_n(&audioState.remoteMask, mask, __ATOMIC_RELAXED);
}

uint16_t getRemoteMask() {
  return __atomic_load_n(&audioState.remoteMask, __ATOMIC_RELAXED);
}

void setRemoteFreq(uint8_t note, uint32_t freq) {
  if (note < 12) __atomic_store_n(&audioState.remoteFreqs[note], freq, __ATOMIC_RELAXED);
}

uint32_t getRemoteFreq(uint8_t note) {
  if (note < 12) return __atomic_load_n(&audioState.remoteFreqs[note], __ATOMIC_RELAXED);
  return 0;
}

// ===== Hardware =====

void setOutMuxBit(const uint8_t bitIdx, const bool value) {
  digitalWrite(REN_PIN, LOW);
  digitalWrite(RA0_PIN, bitIdx & 0x01);
  digitalWrite(RA1_PIN, bitIdx & 0x02);
  digitalWrite(RA2_PIN, bitIdx & 0x04);
  digitalWrite(OUT_PIN, value);
  digitalWrite(REN_PIN, HIGH);
  delayMicroseconds(2);
  digitalWrite(REN_PIN, LOW);
}

std::bitset<4> readCols() {
  std::bitset<4> r;
  r[0] = digitalRead(C0_PIN);
  r[1] = digitalRead(C1_PIN);
  r[2] = digitalRead(C2_PIN);
  r[3] = digitalRead(C3_PIN);
  return r;
}

void setRow(uint8_t rowIdx) {
  digitalWrite(REN_PIN, LOW);
  digitalWrite(RA0_PIN, (rowIdx & 0x01) ? HIGH : LOW);
  digitalWrite(RA1_PIN, (rowIdx & 0x02) ? HIGH : LOW);
  digitalWrite(RA2_PIN, (rowIdx & 0x04) ? HIGH : LOW);
  digitalWrite(REN_PIN, HIGH);
}

static inline int32_t triFromPhase(uint32_t phase) {
  uint8_t x = (uint8_t)(phase >> 24);
  uint8_t t = (x < 128) ? x : (uint8_t)(255 - x);
  return ((int32_t)t << 1) - 128;
}

// ===== ISR =====

void sampleISR() {
  static uint32_t phaseAccLocal[12]  = {0};
  static uint32_t phaseAccRemote[12] = {0};

  uint16_t lMask = getLocalMask();
  uint16_t rMask = getRemoteMask();

#ifdef TEST_SAMPLE_ISR
  lMask = 0x0FFF;
  rMask = 0x0FFF;
#endif

#if defined(MODE_SENDER)
  analogWrite(OUTR_PIN, 128);
  analogWrite(OUTL_PIN, 128);
  return;
#endif

  int32_t mix    = 0;
  uint8_t voices = 0;

  for (uint8_t n = 0; n < 12; n++) {
    if (lMask & (1u << n)) {
      phaseAccLocal[n] += stepSizes[n];
      mix += triFromPhase(phaseAccLocal[n]);
      voices++;
    }
  }

  for (uint8_t n = 0; n < 12; n++) {
    if (rMask & (1u << n)) {
      uint32_t freq = getRemoteFreq(n);
      phaseAccRemote[n] += freq;
      mix += triFromPhase(phaseAccRemote[n]);
      voices++;
    }
  }

  if (voices == 0) {
    analogWrite(OUTR_PIN, 128);
    analogWrite(OUTL_PIN, 128);
    return;
  }

  mix /= (int32_t)voices;

  uint8_t vol = __atomic_load_n(&sysState.knob3Rotation, __ATOMIC_RELAXED);
  if (vol > 8) vol = 8;
  mix = mix >> (8 - vol);

  int32_t out = mix + 128;
  if (out < 0)   out = 0;
  if (out > 255) out = 255;

  analogWrite(OUTR_PIN, (uint8_t)out);
  analogWrite(OUTL_PIN, (uint8_t)out);
}

void CAN_RX_ISR(void) {
  uint8_t RX_Message_ISR[8] = {0};
  uint32_t ID = 0;
  CAN_RX(ID, RX_Message_ISR);
  if (msgInQ) xQueueSendFromISR(msgInQ, RX_Message_ISR, NULL);
}

void CAN_TX_ISR(void) {
  xSemaphoreGiveFromISR(CAN_TX_Semaphore, NULL);
}

// ===== Tasks =====

// CAN message format (bitmask approach):
// Byte 0: 'M' (mask message type)
// Byte 1: OCTAVE
// Byte 2: low byte of 12-bit key mask
// Byte 3: high nibble of 12-bit key mask
// Bytes 4-7: unused

void scanKeysTask(void *pvParameters) {
  const TickType_t xFrequency = SCAN_PERIOD_MS / portTICK_PERIOD_MS;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint16_t lastMask   = 0;
  uint16_t stableMask = 0;

#ifndef TEST_SCANKEYS
  while (1) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
#endif

    std::bitset<32> localInputs;

    for (uint8_t row = 0; row < 4; row++) {
      setRow(row);
      delayMicroseconds(3);
      std::bitset<4> cols = readCols();
      for (uint8_t col = 0; col < 4; col++) localInputs[row * 4 + col] = cols[col];
    }

#ifdef TEST_SCANKEYS
    // Worst case: all keys pressed, knob turning
    localInputs = 0x00000000;
    localInputs[12] = 1;
    localInputs[13] = 0;
    lastMask = 0x0000;
#endif

    bool A = (localInputs[3 * 4 + 0] != 0);
    bool B = (localInputs[3 * 4 + 1] != 0);
    knob3.update(A, B);

    uint8_t newRot = knob3.read();
    __atomic_store_n(&sysState.knob3Rotation, newRot, __ATOMIC_RELAXED);

    updateInputs(localInputs);

    uint16_t keys12 = (uint16_t)(localInputs.to_ulong() & 0x0FFF);
    uint16_t mask = 0;
    for (uint8_t note = 0; note < 12; note++) {
      bool pressed = (((keys12 >> note) & 0x1) == 0);
      if (pressed) mask |= (1u << note);
    }

    if (mask == lastMask) stableMask = mask;
    lastMask = mask;

    setLocalMask(stableMask);

#if !defined(MODE_RECEIVER)
    // Send one bitmask frame per scan period — worst case is always 1 CAN message
    uint8_t TX_Message[8] = {0};
    TX_Message[0] = 'M';                          // mask message type
    TX_Message[1] = FIXED_OCTAVE;
    TX_Message[2] = (uint8_t)(stableMask & 0xFF); // low byte of mask
    TX_Message[3] = (uint8_t)(stableMask >> 8);   // high nibble of mask
    xQueueSend(msgOutQ, TX_Message, portMAX_DELAY);
#endif

    #if defined(StackSizeScanKeys)
      UBaseType_t stackLeft = uxTaskGetStackHighWaterMark(NULL);
      Serial.print("scanKeys stack left: ");
      Serial.println(stackLeft);
    #endif

#ifndef TEST_SCANKEYS
  }
#endif
}

void displayUpdateTask(void *pvParameters) {
  const TickType_t xFrequency = DISPLAY_PERIOD_MS / portTICK_PERIOD_MS;
  TickType_t xLastWakeTime = xTaskGetTickCount();

#ifndef TEST_DISPLAY
  while (1) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
#endif

    std::bitset<32> inputsCopy;
    uint8_t rxCopy[8];
    uint32_t key12;
    uint8_t vol;
    uint16_t lMask;
    uint16_t rMask;

#ifdef TEST_DISPLAY
    inputsCopy = 0xFFFFFFFF;
    key12 = 0xFFF;
    vol = 8;
    lMask = 0xFFF;
    rMask = 0xFFF;
    rxCopy[0] = 'M';
    rxCopy[1] = 255;
    rxCopy[2] = 255;
    rxCopy[3] = 255;
    for (int i = 4; i < 8; i++) rxCopy[i] = 0;
#else
    getInputsAndRxMsg(inputsCopy, rxCopy);
    key12 = inputsCopy.to_ulong() & 0x0FFF;
    vol   = __atomic_load_n(&sysState.knob3Rotation, __ATOMIC_RELAXED);
    lMask = getLocalMask();
    rMask = getRemoteMask();
#endif

    const char* noteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);

    u8g2.setCursor(0, 10);
    u8g2.print("Notes:");

    int xPos = 40;
    for (int i = 0; i < 12; i++) {
      if (lMask & (1u << i)) {
        u8g2.setCursor(xPos, 10);
        u8g2.print(noteNames[i]);
        xPos += 16;
        if (xPos > 120) break;
      }
    }

    u8g2.setCursor(0, 22);
    u8g2.print("Vol:");
    u8g2.print(vol);

    u8g2.setCursor(50, 22);
    u8g2.print("Oct:");
    u8g2.print(FIXED_OCTAVE);

    u8g2.setCursor(0, 30);
    u8g2.print("L:");
    u8g2.print(lMask, HEX);

    u8g2.setCursor(50, 30);
    u8g2.print("R:");
    u8g2.print(rMask, HEX);

    u8g2.setCursor(100, 30);
    u8g2.print((char)rxCopy[0]);

    u8g2.sendBuffer();
    digitalToggle(LED_BUILTIN);

    #if defined(StackSizeDisplayUpdate)
      UBaseType_t stackLeft = uxTaskGetStackHighWaterMark(NULL);
      Serial.print("displayUpdate stack left: ");
      Serial.println(stackLeft);
    #endif

#ifndef TEST_DISPLAY
  }
#endif
}

// decodeTask now handles 'M' (mask) messages instead of 'P'/'R' per note
void decodeTask(void *pvParameters) {
  uint8_t RX_Message[8] = {0};

#ifndef TEST_DECODE
  while (1) {
#endif

    xQueueReceive(msgInQ, RX_Message, portMAX_DELAY);
    updateRxMsg(RX_Message);

#if defined(MODE_SENDER) && !defined(TEST_DECODE)
    continue;
#else
    uint8_t type      = RX_Message[0];
    uint8_t rcvOctave = RX_Message[1];

    if (type == (uint8_t)'M') {
      // Reconstruct 12-bit mask from bytes 2 and 3
      uint16_t newMask = (uint16_t)RX_Message[2] | ((uint16_t)RX_Message[3] << 8);
      newMask &= 0x0FFF; // ensure only 12 bits

      // Update remote frequencies for any newly active notes
      int8_t octaveShift = (int8_t)rcvOctave - 4;
      for (uint8_t note = 0; note < 12; note++) {
        if (newMask & (1u << note)) {
          uint32_t freq = stepSizes[note];
          if (octaveShift > 0) freq = freq << octaveShift;
          else if (octaveShift < 0) freq = freq >> (-octaveShift);
          setRemoteFreq(note, freq);
        }
      }

      setRemoteMask(newMask);
    }
#endif

    #if defined(StackSizeDecode)
      UBaseType_t stackLeft = uxTaskGetStackHighWaterMark(NULL);
      Serial.print("decode stack left: ");
      Serial.println(stackLeft);
    #endif

#ifndef TEST_DECODE
  }
#endif
}

void CAN_TX_Task(void *pvParameters) {
  uint8_t msgOut[8] = {0};

#ifndef TEST_CANTX
  while (1) {
#endif

#if defined(MODE_RECEIVER) && !defined(TEST_CANTX)
    vTaskDelay(10 / portTICK_PERIOD_MS);
#else
    xQueueReceive(msgOutQ, msgOut, portMAX_DELAY);
    xSemaphoreTake(CAN_TX_Semaphore, portMAX_DELAY);
    CAN_TX(CAN_ID, msgOut);
#endif

    #if defined(StackSizeCAN_TX)
      UBaseType_t stackLeft = uxTaskGetStackHighWaterMark(NULL);
      Serial.print("CAN_TX stack left: ");
      Serial.println(stackLeft);
    #endif

#ifndef TEST_CANTX
  }
#endif
}

// ===== Initialization =====

static void initPinsAndDisplay() {
  pinMode(RA0_PIN, OUTPUT);
  pinMode(RA1_PIN, OUTPUT);
  pinMode(RA2_PIN, OUTPUT);
  pinMode(REN_PIN, OUTPUT);
  pinMode(OUT_PIN, OUTPUT);

  pinMode(C0_PIN, INPUT_PULLUP);
  pinMode(C1_PIN, INPUT_PULLUP);
  pinMode(C2_PIN, INPUT_PULLUP);
  pinMode(C3_PIN, INPUT_PULLUP);

  pinMode(OUTL_PIN, OUTPUT);
  pinMode(OUTR_PIN, OUTPUT);

  pinMode(JOYX_PIN, INPUT);
  pinMode(JOYY_PIN, INPUT);

  pinMode(LED_BUILTIN, OUTPUT);

  setOutMuxBit(DRST_BIT, LOW);
  delayMicroseconds(2);
  setOutMuxBit(DRST_BIT, HIGH);
  u8g2.begin();
  setOutMuxBit(DEN_BIT, HIGH);
  setOutMuxBit(KNOB_MODE, HIGH);
  setOutMuxBit(HKOE_BIT, HIGH);
  setOutMuxBit(HKOW_BIT, HIGH);
}

void setup() {
  Serial.begin(9600);

  initPinsAndDisplay();

  sysState.inputs.reset();
  sysState.knob3Rotation = 8;
  for (int i = 0; i < 8; i++) sysState.rxMsg[i] = 0;
  sysState.mutex   = xSemaphoreCreateMutex();
  CAN_TX_Semaphore = xSemaphoreCreateCounting(3, 3);

  analogWriteResolution(8);

  // Queue size of 4 is sufficient — at most 1 message per 20ms scan
  msgInQ  = xQueueCreate(4, 8);
  msgOutQ = xQueueCreate(4, 8);

  CAN_Init(false);
  setCANFilter(CAN_ID, 0x7ff);

  #ifndef DISABLE_THREADS
  CAN_RegisterRX_ISR(CAN_RX_ISR);
  CAN_RegisterTX_ISR(CAN_TX_ISR);
  #endif

  CAN_Start();

  sampleTimer.setOverflow(AUDIO_FS_HZ, HERTZ_FORMAT);

  #ifndef DISABLE_THREADS
  sampleTimer.attachInterrupt(sampleISR);
  #endif

  sampleTimer.resume();

#ifndef DISABLE_THREADS
  xTaskCreate(scanKeysTask,      "scanKeys", 128, NULL, 3, NULL);
  xTaskCreate(displayUpdateTask, "display",  180, NULL, 1, NULL);
  xTaskCreate(decodeTask,        "decode",   128, NULL, 2, NULL);
  xTaskCreate(CAN_TX_Task,       "canTx",    128, NULL, 2, NULL);

  vTaskStartScheduler();
#endif

// ===== Timing Tests =====

#ifdef TEST_SCANKEYS
  delay(2000);
  msgOutQ = xQueueCreate(36, 8);
  uint32_t startTime = micros();
  for (int i = 0; i < 32; i++) scanKeysTask(NULL);
  Serial.println(micros() - startTime);
  while (1);
#endif

#ifdef TEST_DISPLAY
  delay(2000);
  uint32_t startTime = micros();
  for (int i = 0; i < 32; i++) displayUpdateTask(NULL);
  Serial.println(micros() - startTime);
  while (1);
#endif

#ifdef TEST_DECODE
  delay(2000);
  msgInQ = xQueueCreate(4, 8);
  // Worst case: mask message with all 12 notes and max octave shift
  uint8_t testMsg[8] = {'M', 7, 0xFF, 0x0F, 0, 0, 0, 0};
  for (int i = 0; i < 4; i++) xQueueSend(msgInQ, testMsg, 0);

  uint32_t startTime = micros();
  for (int i = 0; i < 4; i++) decodeTask(NULL);
  Serial.println(micros() - startTime);
  while (1);
#endif

#ifdef TEST_CANTX
  delay(2000);
  msgOutQ = xQueueCreate(4, 8);
  CAN_TX_Semaphore = xSemaphoreCreateCounting(4, 4);

  // Worst case: full mask message
  uint8_t testMsg[8] = {'M', FIXED_OCTAVE, 0xFF, 0x0F, 0, 0, 0, 0};
  for (int i = 0; i < 4; i++) xQueueSend(msgOutQ, testMsg, 0);

  uint32_t startTime = micros();
  for (int i = 0; i < 4; i++) CAN_TX_Task(NULL);
  Serial.println(micros() - startTime);
  while (1);
#endif

#ifdef TEST_SAMPLE_ISR
  delay(2000);

  setLocalMask(0x0FFF);
  setRemoteMask(0x0FFF);
  for (int i = 0; i < 12; i++) setRemoteFreq(i, stepSizes[i]);
  sysState.knob3Rotation = 8;

  uint32_t startTime = micros();
  for (int i = 0; i < 1000; i++) sampleISR();
  Serial.println(micros() - startTime);
  while (1);
#endif

}

void loop() {}