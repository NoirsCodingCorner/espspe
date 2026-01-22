/*#include <Arduino.h>
#include <SPI.h>
#include <esp_mac.h>
#include "adin1110_regs.h"

// ==========================================
// CONFIG
// ==========================================
#define PIN_SCLK 18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_CS   5
#define PIN_RST  4
#define PIN_INT  32

// PWM SETTINGS (Dimming)
#define PIN_LED      2   
#define PWM_CHANNEL  0
#define PWM_FREQ     5000
#define PWM_RES      8   // 8-bit resolution (0-255)

SPIClass adinSpi(VSPI);
uint32_t currentSpiFreq = 1000000;

uint8_t ioBuffer[2048] __attribute__ ((aligned (4)));
uint8_t MY_MAC[6];

// ==========================================
// ANIMATION ENGINE STATE
// ==========================================
#define MAX_STEPS 32

struct AnimStep {
    int duration;   // Milliseconds to transition
    int brightness; // Target brightness (0-255)
};

AnimStep sequence[MAX_STEPS];
int seqLength = 0;
int currentStepIdx = 0;

unsigned long stepStartTime = 0;
float startBrightness = 0; // Float for smooth calculation

// ==========================================
// DRIVER CAPSULE
// ==========================================
uint8_t calculateCRC(uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07; else crc <<= 1;
        }
    }
    return crc;
}

uint32_t readReg(uint16_t regAddr) {
    uint16_t header = 0x8000 | (regAddr & 0x1FFF);
    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };
    
    adinSpi.beginTransaction(SPISettings(currentSpiFreq, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2)); adinSpi.transfer(0);
    uint8_t b[4];
    for(int i=0; i<4; i++) b[i] = adinSpi.transfer(0);
    adinSpi.transfer(0);
    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();
    return ((uint32_t)b[0]<<24) | ((uint32_t)b[1]<<16) | ((uint32_t)b[2]<<8) | b[3];
}

void writeReg(uint16_t regAddr, uint32_t data) {
    uint16_t header = 0xA000 | (regAddr & 0x1FFF);
    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };
    uint8_t dataBuf[4] = { (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };

    adinSpi.beginTransaction(SPISettings(currentSpiFreq, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2));
    adinSpi.transfer(dataBuf[0]); adinSpi.transfer(dataBuf[1]); adinSpi.transfer(dataBuf[2]); adinSpi.transfer(dataBuf[3]);
    adinSpi.transfer(calculateCRC(dataBuf, 4));
    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();
}

void adin_init() {
    pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_RST, OUTPUT); 
    adinSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, PIN_CS);

    digitalWrite(PIN_RST, LOW); delay(20);
    digitalWrite(PIN_RST, HIGH); delay(100);

    readReg(ADIN1110_PHY_ID_REG);

    esp_read_mac(MY_MAC, ESP_MAC_WIFI_STA);
    MY_MAC[5] = 0xBB; // Receiver
    
    uint32_t macHi = (MY_MAC[0] << 24) | (MY_MAC[1] << 16) | (MY_MAC[2] << 8) | MY_MAC[3];
    uint32_t macLo = (MY_MAC[4] << 8) | MY_MAC[5];
    writeReg(ADIN1110_MAC_ADDR_FILT_UPR_REG(0), macHi);
    writeReg(ADIN1110_MAC_ADDR_FILT_LWR_REG(0), macLo);

    uint32_t cfg2 = readReg(ADIN1110_CONFIG2_REG);
    cfg2 |= ADIN1110_FWD_UNK2HOST_MASK | ADIN1110_CRC_APPEND; 
    writeReg(ADIN1110_CONFIG2_REG, cfg2);
}

char* adin_poll_frame() {
    static uint32_t lastRxCnt = 0;
    uint32_t rxCnt = readReg(ADIN1110_RX_FRM_CNT_REG);
    if (rxCnt == lastRxCnt) return NULL;
    
    lastRxCnt = rxCnt;
    uint32_t fifoSize = readReg(ADIN1110_RX_FSIZE_REG);
    if (fifoSize == 0) return NULL;

    uint16_t header = 0x8000 | (ADIN1110_RX_REG & 0x1FFF);
    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };

    adinSpi.beginTransaction(SPISettings(currentSpiFreq, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2)); adinSpi.transfer(0);
    
    uint32_t readLen = (fifoSize > 2048) ? 2048 : fifoSize;
    adinSpi.transferBytes(NULL, ioBuffer, readLen);
    
    adinSpi.transfer(0);
    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();

    if (readLen <= 16) return NULL;
    if (readLen < 2048) ioBuffer[readLen] = 0; else ioBuffer[2047] = 0;
    return (char*)&ioBuffer[16];
}

// ==========================================
// ANIMATION LOGIC
// ==========================================

void process_sequence_command(char* payload) {
    if (strncmp(payload, "SEQ:", 4) != 0) return;

    Serial.printf(">> [PROG] Parsing Program: %s\n", payload);

    // Reset
    seqLength = 0;
    
    // Parse Pairs: Duration,Brightness
    char* token = strtok(payload + 4, ",");
    while (token != NULL && seqLength < MAX_STEPS) {
        int dur = atoi(token);
        token = strtok(NULL, ",");
        if (token != NULL) {
            int bri = atoi(token);
            sequence[seqLength].duration = dur;
            sequence[seqLength].brightness = bri;
            seqLength++;
        }
        token = strtok(NULL, ",");
    }

    if (seqLength > 0) {
        currentStepIdx = 0;
        stepStartTime = millis();
        // Start from current brightness to avoid jumps
        // (startBrightness is already holding current value)
        Serial.printf(">> [PROG] Loaded %d steps. Starting...\n", seqLength);
    }
}

void run_animation() {
    if (seqLength == 0) return;

    AnimStep target = sequence[currentStepIdx];
    unsigned long now = millis();
    unsigned long elapsed = now - stepStartTime;

    if (elapsed >= target.duration) {
        // Step Finished -> Set Final Brightness
        ledcWrite(PWM_CHANNEL, target.brightness);
        startBrightness = target.brightness; 
        
        // Move to Next Step
        currentStepIdx++;
        if (currentStepIdx >= seqLength) currentStepIdx = 0; // Loop
        
        stepStartTime = now;
        
        // If next step is duration 0, handle instantly in next loop
    } else {
        // Interpolate (Fade)
        // Only if duration > 0, otherwise it's handled above
        if (target.duration > 0) {
            float progress = (float)elapsed / (float)target.duration;
            float newBri = startBrightness + (progress * (target.brightness - startBrightness));
            ledcWrite(PWM_CHANNEL, (int)newBri);
        }
    }
}

// ==========================================
// MAIN
// ==========================================
void setup() {
    Serial.begin(115200); while(!Serial) delay(10);
    Serial.println("\n=== ADIN1110 DIMMER RECEIVER ===");
    
    // SETUP PWM
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
    ledcAttachPin(PIN_LED, PWM_CHANNEL);
    
    // PWM Test (Fade In/Out once)
    for(int i=0; i<255; i+=5) { ledcWrite(PWM_CHANNEL, i); delay(5); }
    for(int i=255; i>0; i-=5) { ledcWrite(PWM_CHANNEL, i); delay(5); }
    ledcWrite(PWM_CHANNEL, 0);

    adin_init();
    Serial.println("Ready for Programs.");
}

void loop() {
    char* msg = adin_poll_frame();
    if (msg != NULL) {
        process_sequence_command(msg);
    }
    run_animation();
    delay(1); // Keep loop tight for smooth anim
}*/