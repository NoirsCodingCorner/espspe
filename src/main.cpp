#include <Arduino.h>
#include <SPI.h>
#include <esp_mac.h>
#include "adin1110_regs.h"

// ==========================================
// KONFIGURATION
// ==========================================
// 1 = SENDER (Sendet Pakete)
// 0 = EMPFÄNGER (Lauscht und zeigt Details)
#define DEVICE_ROLE 0 
// ==========================================

// PINS
#define PIN_SCLK 18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_CS   5
#define PIN_RST  4
#define PIN_INT  32

SPIClass adinSpi(VSPI);
uint32_t currentSpiFreq = 1000000; // 1 MHz Safe Mode

// Globaler Buffer für TX und RX (DMA aligned)
uint8_t ioBuffer[2048] __attribute__ ((aligned (4)));

// MACs
uint8_t MY_MAC[6];
const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ==========================================
// TOOLS
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

// ==========================================
// SPI CORE
// ==========================================
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

// ==========================================
// SENDER LOGIK
// ==========================================
void runSender() {
    static uint32_t pktCount = 0;
    static unsigned long lastTime = 0;

    // Sende alle 500ms ein Paket
    if (millis() - lastTime > 500) {
        // 1. Nachricht vorbereiten
        char msg[32];
        sprintf(msg, "HELLO #%d", pktCount);
        pktCount++;

        // 2. Buffer löschen & Header aufbauen
        memset(ioBuffer, 0, 100);
        
        // Internal Header (2 Bytes) - muss 0 sein
        ioBuffer[0] = 0; ioBuffer[1] = 0;
        
        // Ethernet Header
        memcpy(&ioBuffer[2], BROADCAST_MAC, 6);
        memcpy(&ioBuffer[8], MY_MAC, 6);
        ioBuffer[14] = 0x88; ioBuffer[15] = 0xB5; // EtherType
        
        // Payload
        memcpy(&ioBuffer[16], msg, strlen(msg));
        
        // Länge berechnen (Min 64 Bytes Ethernet + 2 Bytes Internal Header)
        uint32_t dataLen = 16 + strlen(msg); // EthHeader + Payload
        uint32_t ethLen = (dataLen < 64) ? 64 : dataLen;
        uint32_t spiTotalLen = ethLen + 2; // +2 Internal Header
        
        // SPI Padding (Alignment auf 4 Bytes)
        if (spiTotalLen % 4 != 0) spiTotalLen += (4 - (spiTotalLen % 4));

        // 3. SPI Transfer (Block Mode ohne Lücken!)
        uint16_t header = 0xA000 | (ADIN1110_TX_REG & 0x1FFF);
        uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };

        adinSpi.beginTransaction(SPISettings(currentSpiFreq, MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_CS, LOW);
        
        // Command Header
        adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
        adinSpi.transfer(calculateCRC(headBuf, 2));
        
        // Frame Data (InternalHead + EthHead + Payload + Padding)
        adinSpi.transferBytes(ioBuffer, NULL, spiTotalLen);
        
        // Dummy Byte am Ende (Turnaround/CRC Dummy)
        adinSpi.transfer(0x00);
        
        digitalWrite(PIN_CS, HIGH);
        adinSpi.endTransaction();
        
        // 4. Trigger TX Hardware
        writeReg(ADIN1110_TX_FSIZE_REG, ethLen + 2);

        // Debug Ausgaben
        uint32_t txCnt = readReg(ADIN1110_TX_FRM_CNT_REG);
        uint32_t status = readReg(ADIN1110_STATUS1_REG);
        bool link = (status & 0x01) || (status & 0x04);
        
        Serial.printf("[TX] Packet '%s' gesendet. HW Counter: %d | Link: %s\n", msg, txCnt, link ? "OK" : "NO");
        
        lastTime = millis();
    }
}

// ==========================================
// EMPFÄNGER LOGIK
// ==========================================
void runReceiver() {
    static uint32_t lastRx = 0;
    static uint32_t lastCrc = 0;
    static unsigned long hb = 0;
    
    // Status lesen
    uint32_t rxCnt = readReg(ADIN1110_RX_FRM_CNT_REG);
    uint32_t crcErr = readReg(ADIN1110_RX_CRC_ERR_CNT_REG);
    uint32_t fifoSize = readReg(ADIN1110_RX_FSIZE_REG);
    bool activity = false;

    // 1. Check auf neue Pakete (Hardware Counter)
    if (rxCnt != lastRx) {
        Serial.printf(">> [RX EVENT] Hardware Counter erhöht! Total: %d (+%d)\n", rxCnt, rxCnt - lastRx);
        lastRx = rxCnt;
        activity = true;
    }
    
    // 2. Check auf CRC Fehler
    if (crcErr != lastCrc) {
        Serial.printf("!! [ERROR] CRC Error Counter erhöht! Total: %d (+%d)\n", crcErr, crcErr - lastCrc);
        lastCrc = crcErr;
        activity = true;
    }

    // 3. FIFO Auslesen wenn voll
    if (fifoSize > 0) {
        Serial.printf(">> [FIFO] %d Bytes warten. Lese aus...\n", fifoSize);
        
        // Lesen vorbereiten
        uint16_t header = 0x8000 | (ADIN1110_RX_REG & 0x1FFF);
        uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };

        adinSpi.beginTransaction(SPISettings(currentSpiFreq, MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_CS, LOW);
        
        adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
        adinSpi.transfer(calculateCRC(headBuf, 2));
        adinSpi.transfer(0); // Turnaround
        
        // Daten lesen (Alles auf einmal)
        uint32_t readLen = (fifoSize > 2048) ? 2048 : fifoSize;
        adinSpi.transferBytes(NULL, ioBuffer, readLen);
        
        adinSpi.transfer(0); // Footer
        digitalWrite(PIN_CS, HIGH);
        adinSpi.endTransaction();

        // Inhalt anzeigen (Hex + ASCII)
        Serial.print("   DATA HEX: ");
        for(int i=0; i<16 && i<readLen; i++) Serial.printf("%02X ", ioBuffer[i]);
        Serial.println("...");

        // Offset: 2 Byte Internal Header, dann 14 Byte Eth Header -> Payload ab Index 16
        if (readLen > 16) {
            Serial.print("   PAYLOAD:  ");
            for(int i=16; i<48 && i<readLen; i++) {
                char c = ioBuffer[i];
                if (c >= 32 && c <= 126) Serial.print(c); else Serial.print(".");
            }
            Serial.println();
        }
        activity = true;
    }

    // Heartbeat (Lebenszeichen)
    if (millis() - hb > 3000) {
        if (!activity) {
            uint32_t s1 = readReg(ADIN1110_STATUS1_REG);
            bool link = (s1 & 0x01) || (s1 & 0x04);
            Serial.printf("[IDLE] Link: %s | CRC Err: %d | RX Frames: %d\n", link?"OK":"NO", crcErr, rxCnt);
        }
        hb = millis();
    }
    delay(20);
}

// ==========================================
// SETUP & MAIN
// ==========================================
void setup() {
    Serial.begin(115200); while(!Serial) delay(10);
    Serial.println("\n=== ADIN1110 UNIFIED DEBUGGER ===");
    
    // Pins
    pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_RST, OUTPUT); 
    adinSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, PIN_CS);

    // Hardware Reset
    digitalWrite(PIN_RST, LOW); delay(20);
    digitalWrite(PIN_RST, HIGH); delay(100);

    // ID Check
    uint32_t id = readReg(ADIN1110_PHY_ID_REG);
    Serial.printf("PHY ID: 0x%08X\n", id);

    // MAC Setup
    esp_read_mac(MY_MAC, ESP_MAC_WIFI_STA);
    
    if (DEVICE_ROLE == 1) {
        MY_MAC[5] = 0xAA; 
        Serial.println(">>> ROLLE: SENDER <<<");
    } else {
        MY_MAC[5] = 0xBB;
        Serial.println(">>> ROLLE: EMPFÄNGER <<<");
    }
    
    // MAC Filter setzen
    uint32_t macHi = (MY_MAC[0] << 24) | (MY_MAC[1] << 16) | (MY_MAC[2] << 8) | MY_MAC[3];
    uint32_t macLo = (MY_MAC[4] << 8) | MY_MAC[5];
    writeReg(ADIN1110_MAC_ADDR_FILT_UPR_REG(0), macHi);
    writeReg(ADIN1110_MAC_ADDR_FILT_LWR_REG(0), macLo);

    // ============================================================
    // WICHTIGE CONFIG (DER FIX!)
    // ============================================================
    uint32_t cfg2 = readReg(ADIN1110_CONFIG2_REG);
    Serial.printf("CONFIG2 (vorher): 0x%04X\n", cfg2);
    
    // 1. Promiscuous Mode (Alles durchlassen)
    cfg2 |= ADIN1110_FWD_UNK2HOST_MASK; 
    
    // 2. CRC Append (Automatische Prüfsumme!) -> Das fehlte bisher!
    cfg2 |= ADIN1110_CRC_APPEND; 
    
    writeReg(ADIN1110_CONFIG2_REG, cfg2);
    Serial.printf("CONFIG2 (nachher): 0x%04X\n", readReg(ADIN1110_CONFIG2_REG));
    // ============================================================
    
    Serial.println("System Start...");
}

void loop() {
    if (DEVICE_ROLE == 1) runSender();
    else runReceiver();
}