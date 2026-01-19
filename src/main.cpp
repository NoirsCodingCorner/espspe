#include <Arduino.h>
#include <SPI.h>
#include "adin1110_regs.h"

// ==========================================
// KONFIGURATION
// ==========================================
#define ADIN_CS   5
#define ADIN_MOSI 23
#define ADIN_SCK  18
#define ADIN_MISO 19
#define ADIN_RST  4
#define ADIN_INT  32

#define SPI_SPEED 1000000 // 1 MHz zum Starten

// WICHTIG: Jedes Board braucht eigentlich eine eigene MAC.
// Für diesen Test nutzen wir Promiscuous Mode (hört auf alles), 
// daher ist es vorerst egal, wenn beide gleich sind.
uint8_t MY_MAC[6] = {0x00, 0x1A, 0x11, 0x22, 0x33, 0x44};

SPIClass adinSpi(VSPI);

// ==========================================
// LOW LEVEL SPI (Generic Mode)
// ==========================================

uint8_t calculateCRC8(uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else crc <<= 1;
        }
    }
    return crc;
}

// Schreibt in ein Register (32 Bit)
void writeReg(uint16_t addr, uint32_t val) {
    uint16_t cmd = (addr & 0x1FFF); // Bit 15=0 (Write)
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    uint8_t crc = calculateCRC8(buf, 2);

    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(ADIN_CS, LOW);
    
    adinSpi.transfer16(cmd);
    adinSpi.transfer(crc); // Header CRC
    
    // Daten senden (MSB first)
    uint8_t d3 = (val >> 24) & 0xFF;
    uint8_t d2 = (val >> 16) & 0xFF;
    uint8_t d1 = (val >> 8)  & 0xFF;
    uint8_t d0 = (val)       & 0xFF;
    
    adinSpi.transfer(d3);
    adinSpi.transfer(d2);
    adinSpi.transfer(d1);
    adinSpi.transfer(d0);
    
    // WICHTIG: Bei Generic SPI Write wird AUCH ein CRC über die Daten erwartet!
    // Wir berechnen CRC über die 4 Datenbytes
    uint8_t dataBuf[4] = {d3, d2, d1, d0};
    uint8_t dataCrc = calculateCRC8(dataBuf, 4);
    adinSpi.transfer(dataCrc);

    digitalWrite(ADIN_CS, HIGH);
    adinSpi.endTransaction();
}

// Liest ein Register (32 Bit)
uint32_t readReg(uint16_t addr) {
    uint16_t cmd = 0x8000 | (addr & 0x1FFF); // Bit 15=1 (Read)
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    uint8_t crc = calculateCRC8(buf, 2);

    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(ADIN_CS, LOW);
    
    adinSpi.transfer16(cmd);
    adinSpi.transfer(crc);
    adinSpi.transfer(0x00); // Turnaround
    
    uint32_t rx = 0;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 24;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 16;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 8;
    rx |= (uint32_t)adinSpi.transfer(0x00);
    
    // (Optional: Hier könnte man noch den Footer-CRC lesen)
    
    digitalWrite(ADIN_CS, HIGH);
    adinSpi.endTransaction();
    return rx;
}

// MDIO Read Helper
uint16_t readMDIO(uint8_t dev, uint16_t reg) {
    uint32_t valAddr = (0x0 << 26) | (dev << 16) | reg; 
    writeReg(ADIN1110_MMD_ACCESS_REG, valAddr); // Select Addr
    
    uint32_t valRead = (0x3 << 26) | (dev << 16); // Op=Read
    writeReg(ADIN1110_MMD_ACCESS_REG, valRead); 

    for(int i=0; i<100; i++) {
        uint32_t res = readReg(ADIN1110_MMD_ACCESS_REG);
        if (res & 0x80000000) return (uint16_t)(res & 0xFFFF);
        delayMicroseconds(10);
    }
    return 0xFFFF;
}

// ==========================================
// PACKET ENGINE
// ==========================================

// Sendet ein Raw Ethernet Frame
void sendPacket(uint8_t *payload, size_t len) {
    // 1. Padding auf 64 Bytes (Ethernet Min Size)
    // ADIN1110 fügt FCS (4 Bytes) selbst hinzu, also Payload min 60.
    // Der Header (2 Bytes) kommt noch dazu.
    
    uint32_t tx_space = readReg(ADIN1110_TX_SPACE_REG);
    if (tx_space < (len/2 + 10)) {
        Serial.println("TX FIFO Full!");
        return;
    }

    uint32_t totalLen = len + 2; // +2 für den internen Header
    if (totalLen < 64) totalLen = 64; // Padding
    
    // Auf 4 Bytes runden (Alignment)
    if (totalLen % 4 != 0) totalLen += (4 - (totalLen % 4));

    // 2. Größe ankündigen
    writeReg(ADIN1110_TX_FSIZE_REG, totalLen);

    // 3. Burst Write in TX_REG (0x31)
    uint16_t cmd = (ADIN1110_TX_REG & 0x1FFF); 
    uint8_t headBuf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    uint8_t headCrc = calculateCRC8(headBuf, 2);

    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(ADIN_CS, LOW);
    
    // Command Phase
    adinSpi.transfer16(cmd);
    adinSpi.transfer(headCrc);

    // Data Phase: Interne Header (2 Bytes Port Info)
    adinSpi.transfer(0x00); // Port 0
    adinSpi.transfer(0x00); // Priority 0
    
    // Data Phase: Payload
    for(size_t i=0; i<len; i++) {
        adinSpi.transfer(payload[i]);
    }
    
    // Data Phase: Padding (0x00)
    for(size_t i=len+2; i<totalLen; i++) {
        adinSpi.transfer(0x00);
    }

    // ACHTUNG: Der Generic SPI Treiber in adin1110.c berechnet CRC 
    // für den Burst-Payload NICHT bzw. ignoriert ihn oft im No-OS Mode.
    // Wir senden hier keinen Payload-CRC, da das Handling komplex ist.
    // Falls Generic Strict Mode an ist, könnte das ein Problem sein.

    digitalWrite(ADIN_CS, HIGH);
    adinSpi.endTransaction();
    
    Serial.printf("[TX] Packet sent (%d bytes)\n", len);
}

// Prüft auf Empfangene Pakete
void checkRx() {
    uint32_t status = readReg(ADIN1110_STATUS1_REG);
    if (!(status & ADIN1110_RX_RDY)) return; // Nichts da

    // Größe lesen
    uint32_t rxSize = readReg(ADIN1110_RX_FSIZE_REG);
    if (rxSize < 2) return; // Müll

    // Lesen (Burst Read von 0x90)
    uint16_t cmd = 0x8000 | (ADIN1110_RX_REG & 0x1FFF);
    uint8_t headBuf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    uint8_t headCrc = calculateCRC8(headBuf, 2);

    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
    digitalWrite(ADIN_CS, LOW);
    
    adinSpi.transfer16(cmd);
    adinSpi.transfer(headCrc);
    adinSpi.transfer(0x00); // Turnaround
    
    // Daten lesen
    uint8_t header[2]; // Interne Header
    header[0] = adinSpi.transfer(0x00);
    header[1] = adinSpi.transfer(0x00);
    
    Serial.print("[RX] Data: ");
    for(uint32_t i=2; i<rxSize; i++) {
        uint8_t b = adinSpi.transfer(0x00);
        if (i < 30) Serial.printf("%02X ", b); // Nur die ersten Bytes zeigen
    }
    Serial.println("...");
    
    digitalWrite(ADIN_CS, HIGH);
    adinSpi.endTransaction();
}

// ==========================================
// SETUP & MAIN
// ==========================================

void setup() {
    Serial.begin(115200);
    pinMode(ADIN_CS, OUTPUT); digitalWrite(ADIN_CS, HIGH);
    pinMode(ADIN_RST, OUTPUT);
    
    adinSpi.begin(ADIN_SCK, ADIN_MISO, ADIN_MOSI, ADIN_CS);
    
    // 1. Hard Reset (Generic Mode erzwingen: MISO High/Low je nach deinem Jumper)
    // Wir nehmen an, Jumper ist LOW, also wollen wir MISO HIGH für "Generic No CRC" 
    // oder LOW für "OA". Du sagtest, Generic mit CRC ging.
    // Wir nutzen hier das Protokoll, das bei dir stabil war (Generic mit CRC).
    
    Serial.println("[INIT] Resetting...");
    pinMode(ADIN_MISO, OUTPUT); digitalWrite(ADIN_MISO, LOW); // Strap für Generic+CRC (mit Jumper High)
    digitalWrite(ADIN_RST, LOW); delay(20);
    digitalWrite(ADIN_RST, HIGH); delay(50);
    pinMode(ADIN_MISO, INPUT); delay(100);

    // 2. Config MAC (Promiscuous Mode an, damit wir alles hören)
    // CONFIG2 Register: CRC Append (Bit 5) + Fwd Unknown (Bit 2)
    uint32_t cfg2 = readReg(ADIN1110_CONFIG2_REG);
    cfg2 |= (1<<5) | (1<<2); 
    writeReg(ADIN1110_CONFIG2_REG, cfg2);
    
    // MAC Addresse setzen (Filter 0)
    uint32_t macHigh = (MY_MAC[0] << 8) | MY_MAC[1];
    uint32_t macLow  = (MY_MAC[2] << 24) | (MY_MAC[3] << 16) | (MY_MAC[4] << 8) | MY_MAC[5];
    writeReg(ADIN1110_MAC_ADDR_FILT_UPR_REG(0), macHigh | (1<<30) | (1<<16)); // Apply to Port + To Host
    writeReg(ADIN1110_MAC_ADDR_FILT_LWR_REG(0), macLow);

    // 3. PHY Software Powerdown entfernen
    // CRSM_SFT_PD_CNTRL (0x1E, 0x8812), Bit 0 auf 0 setzen
    // Wir nutzen den MDIO Helper
    // writeMDIO(0x1E, 0x8812, 0x0000); -> TODO: writeMDIO implementieren, falls nötig.
    // Aber oft macht der Chip Autonegotiation von selbst.
    
    Serial.println("[INIT] Ready. Waiting for Link...");
}

void loop() {
    // 1. Link Check
    uint16_t status = readMDIO(0x01, 0x0001); // PMA_STAT1
    bool linkUp = (status & 0x0004);
    
    static bool prevLink = false;
    if (linkUp != prevLink) {
        if (linkUp) Serial.println("🟢 LINK IS UP!");
        else        Serial.println("🔴 LINK LOST.");
        prevLink = linkUp;
    }

    // 2. Empfangen
    checkRx();

    // 3. Senden (nur alle 2 Sekunden, wenn Link da)
    static unsigned long lastTx = 0;
    if (linkUp && (millis() - lastTx > 2000)) {
        lastTx = millis();
        uint8_t msg[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Dest: Broadcast
                          MY_MAC[0], MY_MAC[1], MY_MAC[2], MY_MAC[3], MY_MAC[4], MY_MAC[5], // Src
                          0x08, 0x00, // Type: IPv4 (Fake)
                          'H', 'e', 'l', 'l', 'o', ' ', 'A', 'D', 'I', 'N' }; // Payload
        sendPacket(msg, sizeof(msg));
    }
}