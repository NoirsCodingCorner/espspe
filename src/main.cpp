#include <Arduino.h>
#include <SPI.h>
#include <esp_mac.h>      // Für automatische MAC
#include "adin1110_regs.h"

// ==========================================
// ROLLE FESTLEGEN
// ==========================================
// 1 = SENDER (Sendet alle 2 Sekunden)
// 2 = EMPFÄNGER (Wartet und blinkt bei Empfang)
#define DEVICE_ROLE 1  
// ==========================================

// Pins
#define PIN_SCLK 18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_CS   5
#define PIN_RST  4
#define PIN_INT  32

#define SPI_SPEED 1000000 // 1 MHz

SPIClass adinSpi(VSPI);

// Globale MAC Variablen
uint8_t MY_MAC[6]; 
const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ==========================================
// CRC HELPER (WICHTIG!)
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
// SPI CORE (FIXED: Generic Mode with CRC)
// ==========================================

void performHardwareReset() {
    digitalWrite(PIN_RST, LOW); delay(20);
    digitalWrite(PIN_RST, HIGH); delay(100); 
}

// Register Lesen
uint32_t readReg(uint16_t regAddr) {
    // FIX 1: OpCode 0x8000 für Read
    uint16_t header = 0x8000 | (regAddr & 0x1FFF); 
    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };
    
    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    
    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2)); // Header CRC senden!
    adinSpi.transfer(0x00); // Turnaround
    
    // Daten lesen
    uint8_t b[4];
    for(int i=0; i<4; i++) b[i] = adinSpi.transfer(0x00);
    uint32_t val = ((uint32_t)b[0]<<24) | ((uint32_t)b[1]<<16) | ((uint32_t)b[2]<<8) | b[3];
    
    adinSpi.transfer(0x00); // Footer CRC lesen (ignorieren wir)
    
    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();
    return val;
}

// Register Schreiben
void writeReg(uint16_t regAddr, uint32_t data) {
    // FIX 1: OpCode 0xA000 für Write (Generic SPI Write)
    uint16_t header = 0xA000 | (regAddr & 0x1FFF);

    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };
    uint8_t dataBuf[4] = { (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };

    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    
    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2)); // Header CRC
    
    adinSpi.transfer(dataBuf[0]); adinSpi.transfer(dataBuf[1]); adinSpi.transfer(dataBuf[2]); adinSpi.transfer(dataBuf[3]);
    adinSpi.transfer(calculateCRC(dataBuf, 4)); // FIX 2: Data CRC Senden!
    
    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();
}

// MDIO Helper
void waitForMdioDone() {
    for(int i=0; i<1000; i++) {
        if (readReg(ADIN1110_MDIOACC(0)) & 0x80000000) return; // TRDONE
        delayMicroseconds(10);
    }
}

void writePhyRegister(uint8_t devAddr, uint16_t regAddr, uint16_t data) {
    uint32_t cmdAddr = 0x00200000 | ((uint32_t)devAddr << 16) | regAddr;
    writeReg(ADIN1110_MDIOACC(0), cmdAddr); waitForMdioDone();
    
    uint32_t cmdWrite = 0x04200000 | ((uint32_t)devAddr << 16) | data;
    writeReg(ADIN1110_MDIOACC(1), cmdWrite); waitForMdioDone();
}

// ==========================================
// PACKET ENGINE (FIXED)
// ==========================================

void sendBlinkPacket() {
    // FIX 3: Buffer groß genug für Padding (Min 64 Bytes + 2 Overhead)
    uint8_t buffer[70]; 
    memset(buffer, 0, 70); // Alles mit 0 füllen (Padding)

    // Ethernet Frame Aufbau
    memcpy(&buffer[0], BROADCAST_MAC, 6); // Ziel: Broadcast
    memcpy(&buffer[6], MY_MAC, 6);        // Quelle: Eigene ESP MAC
    buffer[12] = 0x88; buffer[13] = 0xB5; // EtherType (Experimental)
    memcpy(&buffer[14], "BLINK", 5);      // Payload

    // Längen berechnen
    uint32_t ethLen = 64; // Ethernet Minimum
    uint32_t spiLen = ethLen + 2; // +2 Bytes interner ADIN Header
    if (spiLen % 4 != 0) spiLen += (4 - (spiLen % 4)); // 4-Byte Alignment

    // Header für FIFO Write (OpCode 0xA000 | TX_REG)
    uint16_t header = 0xA000 | (ADIN1110_TX_REG & 0x1FFF);
    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };

    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);

    // 1. Befehl
    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2));

    // 2. Interner Header (2 Bytes)
    adinSpi.transfer(0x00); adinSpi.transfer(0x00);
    
    // 3. Frame Data
    for (int i = 0; i < ethLen; i++) adinSpi.transfer(buffer[i]);
    
    // 4. Padding (Alignment)
    for (int i = 0; i < (spiLen - ethLen - 2); i++) adinSpi.transfer(0x00);

    // FIX 4: Dummy CRC Byte senden
    // (Der Chip braucht hier ein Byte, prüft es aber oft nicht strikt, 0x00 reicht meistens)
    adinSpi.transfer(0x00); 

    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();

    // Senden auslösen (Größe = Ethernet Frame + 2 Header Bytes)
    writeReg(ADIN1110_TX_FSIZE_REG, ethLen + 2);
    Serial.println("[TX] BLINK gesendet.");
}

void checkRxPacket() {
    uint32_t rxSize = readReg(ADIN1110_RX_FSIZE_REG);
    if (rxSize < 16) return;

    // Burst Read vorbereiten
    uint16_t header = 0x8000 | (ADIN1110_RX_REG & 0x1FFF);
    uint8_t headBuf[2] = { (uint8_t)(header >> 8), (uint8_t)(header & 0xFF) };

    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);

    adinSpi.transfer(headBuf[0]); adinSpi.transfer(headBuf[1]);
    adinSpi.transfer(calculateCRC(headBuf, 2));
    adinSpi.transfer(0x00); // Turnaround

    // Daten lesen
    uint8_t rxBuffer[128];
    uint32_t readLen = (rxSize > 128) ? 128 : rxSize;
    for(int i=0; i < readLen; i++) {
        rxBuffer[i] = adinSpi.transfer(0x00);
    }
    adinSpi.transfer(0x00); // Footer CRC

    digitalWrite(PIN_CS, HIGH);
    adinSpi.endTransaction();

    // Analyse: Byte 0+1 = Interner Header. Ethernet Frame beginnt ab Index 2.
    // EtherType ist bei Index 14+2 = 16. Payload ab Index 16+2?? 
    // Nein: Header(2) + Dest(6) + Src(6) + Type(2) = 16 Bytes Offset bis Payload.
    // Also Payload ist bei rxBuffer[16].
    
    if (strncmp((char*)&rxBuffer[16], "BLINK", 5) == 0) {
        Serial.println("!!! BLINK EMPFANGEN !!!");
        // LED Effekt
        writePhyRegister(0x1E, 0x8C82, 0x008F); // LED Blink
        delay(200);
        writePhyRegister(0x1E, 0x8C82, 0x0001); // LED Normal
    }
}

// ==========================================
// MAIN SETUP & LOOP
// ==========================================

void setMacAddress(uint8_t *mac) {
    uint32_t macHi = (mac[0] << 24) | (mac[1] << 16) | (mac[2] << 8) | mac[3];
    uint32_t macLo = (mac[4] << 8) | mac[5];
    // Generic SPI: Adressen sind Byte-Swapped für Register Write? 
    // Nein, mein writeReg nimmt uint32 und schiebt MSB zuerst raus. Das passt.
    writeReg(ADIN1110_MAC_ADDR_FILT_UPR_REG(0), macHi);
    writeReg(ADIN1110_MAC_ADDR_FILT_LWR_REG(0), macLo);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_RST, OUTPUT); 
    adinSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, PIN_CS);

    Serial.println("\n--- ADIN1110 CLEAN SETUP (FIXED) ---");
    performHardwareReset();

    uint32_t id = readReg(ADIN1110_PHY_ID_REG);
    if (id != 0x0283BC91) {
        Serial.printf("FEHLER: ID 0x%08X\n", id);
        while(1) delay(100);
    }
    Serial.println("ID OK.");

    // MAC vom ESP lesen & anpassen
    esp_read_mac(MY_MAC, ESP_MAC_WIFI_STA);
    // Letztes Byte je nach Rolle ändern, damit sie sich unterscheiden
    if(DEVICE_ROLE == 1) MY_MAC[5] = 0xAA; 
    else MY_MAC[5] = 0xBB;
    
    setMacAddress(MY_MAC);
    Serial.printf("MAC: ...:%02X\n", MY_MAC[5]);

    Serial.println("Warte auf Link...");
}

void loop() {
    static bool linkWasUp = false;
    uint32_t status = readReg(ADIN1110_STATUS1_REG);
    
    // Generic SPI Mapping Check:
    // Im Kollegen-Code war Bit 2 der Link Status. In der Header File ist es Bit 0.
    // Wir prüfen beides sicherheitshalber.
    bool linkUp = (status & 0x01) || (status & 0x04); 

    if (linkUp && !linkWasUp) Serial.println("🟢 LINK UP!");
    if (!linkUp && linkWasUp) Serial.println("🔴 LINK DOWN!");
    linkWasUp = linkUp;

    // SENDER LOGIK
    if (DEVICE_ROLE == 1 && linkUp) {
        Serial.println("Sende BLINK...");
        sendBlinkPacket();
        delay(2000);
    }

    // EMPFÄNGER LOGIK
    if (DEVICE_ROLE == 2 && linkUp) {
        // Prüfen ob Daten da sind (Bit 0 in Status1 beim Kollegen? Oder RX_RDY Bit 4?)
        // Der Kollege prüft "status & 0x01" für RX Available. 
        // Deine Header sagt RX_RDY ist Bit 4.
        // Generic SPI mapped Bits manchmal anders. Wir prüfen einfach auf beides :)
        if ((status & ADIN1110_RX_RDY) || (status & 0x01)) { 
             checkRxPacket();
        }
        delay(10);
    }
}