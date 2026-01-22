#include <Arduino.h>
#include <SPI.h>
#include <esp_mac.h>
#include <WiFi.h>
#include <WebServer.h>
#include "adin1110_regs.h"

const char* ssid = "ESP32-Dimmer-Control";
const char* password = "12345678";

WebServer server(80);

// ==========================================
// ADIN CONFIG
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
// DRIVER (Standard)
// ==========================================
uint8_t calculateCRC(uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) { if (crc & 0x80) crc = (crc << 1) ^ 0x07; else crc <<= 1; }
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
    MY_MAC[5] = 0xAA; 
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
    ioBuffer[0] = 0; ioBuffer[1] = 0; 
    memcpy(&ioBuffer[2], BROADCAST_MAC, 6);
    memcpy(&ioBuffer[8], MY_MAC, 6);
    ioBuffer[14] = 0x88; ioBuffer[15] = 0xB5; 
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
// WEB & LOGIC
// ==========================================

const char* htmlPage = 
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>LED Program Control</title>"
"<style>body{font-family:sans-serif;text-align:center;padding:20px;background:#222;color:#eee;}"
"button{padding:15px;margin:10px;width:80%;max-width:300px;font-size:18px;border:none;border-radius:5px;cursor:pointer;background:#444;color:white;}"
"button:hover{background:#666;}</style></head>"
"<body><h1>ESP32 Dimmer Control</h1>"
"<button onclick=\"location.href='/prog?id=1'\">1. Smooth Breathing</button>"
"<button onclick=\"location.href='/prog?id=2'\">2. Nervous Pulse</button>"
"<button onclick=\"location.href='/prog?id=3'\">3. Police Strobe</button>"
"<button onclick=\"location.href='/prog?id=4'\">4. Soft Glow (50%)</button>"
"<button onclick=\"location.href='/prog?id=5'\">5. OFF</button>"
"</body></html>";

void handleRoot() {
    server.send(200, "text/html", htmlPage);
}

void handleProg() {
    if (server.hasArg("id")) {
        int id = server.arg("id").toInt();
        
        // Protocol: SEQ:Duration,Brightness,Duration,Brightness...
        
        if (id == 1) {
            // Breathing: Fade to 255 (2s), Fade to 0 (2s)
            adin_send_frame("SEQ:2000,255,2000,0"); 
        }
        else if (id == 2) {
            // Nervous: Fast fade up, instant off
            // Fade to 255 (200ms), Instant to 0 (0ms), Wait 100ms (0 brightness)
            adin_send_frame("SEQ:200,255,0,0,100,0"); 
        }
        else if (id == 3) {
            // Strobe: Instant ON, Wait 50, Instant OFF, Wait 50
            adin_send_frame("SEQ:0,255,50,255,0,0,50,0"); 
        }
        else if (id == 4) {
            // Static Glow: Fade to 50 (1s), then hold (10s)
            adin_send_frame("SEQ:1000,50,10000,50"); 
        }
        else if (id == 5) {
            // OFF: Fade to 0 quickly
            adin_send_frame("SEQ:500,0,1000,0"); 
        }
        
        server.send(200, "text/html", "Program Sent!<br><a href='/'>Back</a>");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void setup() {
    Serial.begin(115200); while(!Serial) delay(10);
    adin_init();

    WiFi.softAP(ssid, password);
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

    server.on("/", handleRoot);
    server.on("/prog", handleProg);
    server.begin();
    Serial.println("HTTP server started");
}

void loop() {
    server.handleClient();
}