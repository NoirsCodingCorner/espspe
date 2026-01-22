#include <Arduino.h>
#include <SPI.h>
#include <esp_mac.h>
#include <WiFi.h>
#include <WebServer.h>
#include "adin1110_regs.h"

// ==========================================
// WIFI CONFIG
// ==========================================
const char* ssid = "ESP32-Blinker-Control";
const char* password = "12345678";

WebServer server(80);

// ==========================================
// ADIN HARDWARE CONFIG
// ==========================================
#define PIN_SCLK 18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_CS   5
#define PIN_RST  4
#define PIN_INT  32

SPIClass adinSpi(VSPI);
uint32_t currentSpiFreq = 1000000;

uint8_t ioBuffer[2048] __attribute__ ((aligned (4)));
uint8_t MY_MAC[6];
const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ==========================================
// LOW LEVEL DRIVER (CAPSULE)
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

    Serial.printf("PHY ID: 0x%08X\n", readReg(ADIN1110_PHY_ID_REG));

    esp_read_mac(MY_MAC, ESP_MAC_WIFI_STA);
    MY_MAC[5] = 0xAA; // Sender Role
    
    uint32_t macHi = (MY_MAC[0] << 24) | (MY_MAC[1] << 16) | (MY_MAC[2] << 8) | MY_MAC[3];
    uint32_t macLo = (MY_MAC[4] << 8) | MY_MAC[5];
    writeReg(ADIN1110_MAC_ADDR_FILT_UPR_REG(0), macHi);
    writeReg(ADIN1110_MAC_ADDR_FILT_LWR_REG(0), macLo);

    uint32_t cfg2 = readReg(ADIN1110_CONFIG2_REG);
    cfg2 |= ADIN1110_FWD_UNK2HOST_MASK | ADIN1110_CRC_APPEND; 
    writeReg(ADIN1110_CONFIG2_REG, cfg2);
}

void adin_send_frame(const char* payload) {
    uint32_t payloadLen = strlen(payload);
    memset(ioBuffer, 0, payloadLen + 20);
    ioBuffer[0] = 0; ioBuffer[1] = 0; // Internal Header
    
    memcpy(&ioBuffer[2], BROADCAST_MAC, 6);
    memcpy(&ioBuffer[8], MY_MAC, 6);
    ioBuffer[14] = 0x88; ioBuffer[15] = 0xB5; // EtherType
    memcpy(&ioBuffer[16], payload, payloadLen);
    
    uint32_t dataLen = 16 + payloadLen;
    uint32_t ethLen = (dataLen < 64) ? 64 : dataLen;
    uint32_t spiTotalLen = ethLen + 2; 
    if (spiTotalLen % 4 != 0) spiTotalLen += (4 - (spiTotalLen % 4));

    uint16_t header = 0xA000 | (ADIN1110_TX_REG & 0x1FFF);
    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };

    adinSpi.beginTransaction(SPISettings(currentSpiFreq, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2));
    adinSpi.transferBytes(ioBuffer, NULL, spiTotalLen);
    adinSpi.transfer(0x00); 
    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();
    
    writeReg(ADIN1110_TX_FSIZE_REG, ethLen + 2);
    Serial.printf("[TX] Sent: %s\n", payload);
}

// ==========================================
// WEB SERVER HANDLERS
// ==========================================

const char* htmlPage = 
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>Blink Control</title>"
"<style>body{font-family:sans-serif;text-align:center;padding:20px;}"
"button{padding:10px 20px;margin:5px;font-size:16px;cursor:pointer;}"
"input{padding:10px;font-size:16px;width:100px;}</style></head>"
"<body><h1>ESP32 ADIN1110 Control</h1>"
"<h3>Set Frequency (Hz)</h3>"
"<form action='/setFreq'><input type='number' name='hz' step='0.1' value='1'><br><br>"
"<button type='submit'>Set Frequency</button></form>"
"<hr><h3>Presets</h3>"
"<button onclick=\"location.href='/preset?id=1'\">Heartbeat</button>"
"<button onclick=\"location.href='/preset?id=2'\">Strobe</button>"
"<button onclick=\"location.href='/preset?id=3'\">Breathe</button>"
"</body></html>";

void handleRoot() {
    server.send(200, "text/html", htmlPage);
}

void handleSetFreq() {
    if (server.hasArg("hz")) {
        float freq = server.arg("hz").toFloat();
        if (freq <= 0) freq = 1;

        // Calculate ON/OFF time in ms (50% duty cycle)
        int periodMs = (int)(1000.0 / freq);
        int halfPeriod = periodMs / 2;
        if (halfPeriod < 10) halfPeriod = 10; // Min safety

        char cmd[64];
        sprintf(cmd, "SEQ:%d,%d", halfPeriod, halfPeriod);
        
        adin_send_frame(cmd);
        server.send(200, "text/html", "OK! Sent: " + String(cmd) + "<br><a href='/'>Back</a>");
    } else {
        server.send(400, "text/plain", "Missing Frequency");
    }
}

void handlePreset() {
    if (server.hasArg("id")) {
        int id = server.arg("id").toInt();
        if (id == 1) adin_send_frame("SEQ:100,200,100,1200"); // Heartbeat
        else if (id == 2) adin_send_frame("SEQ:50,50");       // Strobe
        else if (id == 3) adin_send_frame("SEQ:1000,1000");   // Breathe
        
        server.send(200, "text/html", "Preset Sent!<br><a href='/'>Back</a>");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

// ==========================================
// MAIN LOGIC
// ==========================================

void setup() {
    Serial.begin(115200); while(!Serial) delay(10);
    Serial.println("\n=== ADIN1110 WEB SENDER ===");
    
    // 1. Init ADIN1110
    adin_init();

    // 2. Init WiFi (AP Mode)
    Serial.print("Setting up WiFi AP: ");
    Serial.println(ssid);
    WiFi.softAP(ssid, password);
    
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    // 3. Init Web Server
    server.on("/", handleRoot);
    server.on("/setFreq", handleSetFreq);
    server.on("/preset", handlePreset);
    server.begin();
    Serial.println("HTTP server started");
}

void loop() {
    // Handle Web Requests constantly
    server.handleClient();
}