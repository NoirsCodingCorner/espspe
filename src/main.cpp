#include <Arduino.h>
#include <SPI.h>

// ==========================================
// 1. PIN & KONFIGURATION
// ==========================================
#define ADIN_CS   5
#define ADIN_MOSI 23
#define ADIN_SCK  18
#define ADIN_MISO 19
#define ADIN_RST  4

#define SPI_SPEED 1000000 
const uint32_t ID_ADIN1110 = 0x0283BC91;

SPIClass adinSpi(VSPI);

// ==========================================
// 2. HELPER (CRC)
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

// ==========================================
// 3. CORE SPI (Generic w/ CRC) - FIXED
// ==========================================

void writeSPI(uint16_t addr, uint32_t val) {
    // Write: MSB (Bit 15) muss 0 sein
    uint16_t cmd = (addr & 0x1FFF); 
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    uint8_t crc = calculateCRC8(buf, 2);

    digitalWrite(ADIN_CS, LOW);
    adinSpi.transfer16(cmd);
    adinSpi.transfer(crc);
    adinSpi.transfer((val >> 24) & 0xFF);
    adinSpi.transfer((val >> 16) & 0xFF);
    adinSpi.transfer((val >> 8)  & 0xFF);
    adinSpi.transfer((val)       & 0xFF);
    digitalWrite(ADIN_CS, HIGH);
}

uint32_t readSPI(uint16_t addr) {
    // Read: MSB (Bit 15) muss 1 sein
    uint16_t cmd = 0x8000 | (addr & 0x1FFF); 
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    uint8_t crc = calculateCRC8(buf, 2);

    digitalWrite(ADIN_CS, LOW);
    adinSpi.transfer16(cmd);
    adinSpi.transfer(crc);
    adinSpi.transfer(0x00); // Turnaround
    
    uint32_t rx = 0;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 24;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 16;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 8;
    rx |= (uint32_t)adinSpi.transfer(0x00);
    digitalWrite(ADIN_CS, HIGH);
    return rx;
}

// ==========================================
// 4. MDIO BRIDGE (MIT POLLING FIX)
// ==========================================

uint16_t readPhyReg(uint8_t devAddr, uint16_t regAddr) {
    // 1. Adresse schreiben
    uint32_t valAddr = (0x0 << 26) | (devAddr << 16) | regAddr; 
    writeSPI(0x20, valAddr); 
    
    // 2. Read Befehl schreiben
    uint32_t valRead = (0x3 << 26) | (devAddr << 16);
    writeSPI(0x20, valRead); 
    
    // 3. POLLING LOOP: Warten bis TRDONE (Bit 31) gesetzt ist
    // Dies verhindert, dass wir 0x0000 lesen, wenn der Chip noch beschäftigt ist.
    for(int i=0; i<200; i++) {
        uint32_t result = readSPI(0x20); // Status lesen
        if (result & 0x80000000) {       // Bit 31 gesetzt?
            return (uint16_t)(result & 0xFFFF); // Daten zurückgeben
        }
        delayMicroseconds(10);
    }
    return 0xFFFF; // Timeout (Bridge Error)
}

// ==========================================
// 5. DECODER FUNKTIONEN (LED & STATUS)
// ==========================================

void printLEDStatus(uint16_t val) {
    if (val == 0xFFFF) { Serial.print("READ ERROR"); return; }
    
    // Formatierung so anpassen, dass es in die Tabelle passt (ggf. zweizeilig denken)
    Serial.printf("(Raw: 0x%04X) ", val);
    
    bool led0_en = (val >> 7) & 0x1;
    bool led1_en = (val >> 6) & 0x1;
    uint8_t led0_mode = (val >> 2) & 0x3; 
    uint8_t led1_mode = (val >> 4) & 0x3;

    if(!led0_en && !led1_en) {
        Serial.print("All LEDs Disabled");
        return;
    }

    if(led0_en) {
        Serial.print("L0:");
        if(led0_mode==0) Serial.print("Lnk/Act ");
        else if(led0_mode==1) Serial.print("Lnk ");
        else Serial.print("Act ");
    }
    if(led1_en) {
        Serial.print("| L1:");
        if(led1_mode==0) Serial.print("Lnk/Act");
        else if(led1_mode==1) Serial.print("Lnk");
        else Serial.print("Act");
    }
}

// ==========================================
// 6. VISUALISIERUNG
// ==========================================

void printTable1Header() {
    Serial.println("\n--- TEIL 1: MAC REGISTER (SPI DIRECT) ---");
    Serial.println("+----------------------+--------+------------+------------+---------------------------+");
    Serial.printf("| %-20s | %-6s | %-10s | %-10s | %-25s |\n", "REGISTER NAME", "ADDR", "TX (CMD)", "RX (HEX)", "INTERPRETATION");
    Serial.println("+----------------------+--------+------------+------------+---------------------------+");
}

void printRowMAC(const char* regName, uint8_t addr, uint32_t rxData) {
    char statusBuf[64];
    uint16_t txCmd = 0x8000 | (addr & 0x1FFF); 

    if (addr == 0x01) {
        if (rxData == ID_ADIN1110) strcpy(statusBuf, "✅ ID MATCH");
        else                       strcpy(statusBuf, "❌ WRONG ID");
    } else if (addr == 0x08) {
        if (rxData & 0x80) strcpy(statusBuf, "⚠️ PHY INT");
        else if (rxData & 0x40) strcpy(statusBuf, "✅ RESET COMPLETE");
        else strcpy(statusBuf, "Status OK");
    } else if (addr == 0x00) {
        // MI_CONTROL Interpretation
        if (rxData == 0x10) strcpy(statusBuf, "Default (10BASE-T1L)");
        else sprintf(statusBuf, "Val: %u (Configured)", rxData);
    } else {
         sprintf(statusBuf, "Val: %u", rxData);
    }
    
    Serial.printf("| %-20s | 0x%02X   | 0x%04X     | 0x%08X | %-25s |\n", regName, addr, txCmd, rxData, statusBuf);
}

void printTable2Header() {
    Serial.println("\n--- TEIL 2: PHY REGISTER (VIA MDIO BRIDGE) ---");
    Serial.println("+------+--------+-----------+--------+------------------------------------------+");
    Serial.printf("| %-4s | %-6s | %-9s | %-6s | %-40s |\n", "DEV", "REG", "NAME", "VAL", "DETAIL");
    Serial.println("+------+--------+-----------+--------+------------------------------------------+");
}

void printRowPHY(uint8_t dev, uint16_t reg, const char* name) {
    uint16_t val = readPhyReg(dev, reg);
    
    Serial.printf("| 0x%02X | 0x%04X | %-9s | 0x%04X | ", dev, reg, name, val);
    
    if (val == 0xFFFF) {
        Serial.println("❌ BRIDGE TIMEOUT / ERROR");
    } 
    else if (strcmp(name, "PMA_STAT1") == 0) {
        if (val & 0x0004) Serial.println("🟢 LINK UP (Verbindung OK)");
        else              Serial.println("🔴 LINK DOWN (Warte auf Partner...)");
    }
    else if (strcmp(name, "MSE_VAL") == 0) {
        if (val == 0) Serial.println("No Signal / Perfect Silence");
        else if (val < 0x0600) Serial.printf("✅ Excellent Quality (%d)\n", val);
        else Serial.printf("⚠️ Poor Quality (%d)\n", val);
    }
    else if (strcmp(name, "PHY_STATUS") == 0) {
        if (val & 0x1000) Serial.println("LINK OK (Vendor Bit)");
        else              Serial.println("NO LINK");
    }
    else if (strcmp(name, "LED_CNTRL") == 0) {
        printLEDStatus(val);
        Serial.println();
    }
    else if (strcmp(name, "CRSM_IRQ") == 0) {
        // Interrupt Status Register
        Serial.printf("Raw: %u\n", val);
    }
    else {
        Serial.println("-");
    }
}

// ==========================================
// 7. SETUP & MAIN
// ==========================================

void performSafetyReset() {
    Serial.println("\n[INIT] Safety-Reset (Forcing Generic SPI Mode)...");
    pinMode(ADIN_MISO, OUTPUT); digitalWrite(ADIN_MISO, LOW); 
    digitalWrite(ADIN_RST, LOW); delay(20);
    digitalWrite(ADIN_RST, HIGH); delay(50); 
    pinMode(ADIN_MISO, INPUT); delay(50);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    Serial.println("\n--- ADIN1110 FULL DIAGNOSTIC ---");
    Serial.println("Druecke ENTER zum Starten...");
    while (Serial.available() == 0) delay(100);
    while (Serial.available()) Serial.read();

    pinMode(ADIN_CS, OUTPUT); digitalWrite(ADIN_CS, HIGH);
    pinMode(ADIN_RST, OUTPUT);
    
    adinSpi.begin(ADIN_SCK, ADIN_MISO, ADIN_MOSI, ADIN_CS);
    performSafetyReset();
}

void loop() {
    adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));

    // --- TEIL 1: MAC EBENE ---
    printTable1Header();
    printRowMAC("Identification", 0x01, readSPI(0x01)); // Chip ID
    printRowMAC("MI_CONTROL",     0x00, readSPI(0x00)); // Reset State
    printRowMAC("STATUS_0",       0x08, readSPI(0x08)); // Interrupts / Boot Status
    printRowMAC("IRQ_MASK",       0x0C, readSPI(0x0C)); // Interrupt Maske
    
    // --- TEIL 2: PHY EBENE ---
    printTable2Header();
    // Standard IEEE Register
    printRowPHY(0x01, 0x0001, "PMA_STAT1");  // Link Status (Std)
    printRowPHY(0x01, 0x830B, "MSE_VAL");    // Signal Quality
    printRowPHY(0x01, 0x08F7, "PHY_STATUS"); // Link Status (Vendor)
    
    // Vendor Specific Register
    printRowPHY(0x1E, 0x8C82, "LED_CNTRL");  // LED Config
    printRowPHY(0x1E, 0x0010, "CRSM_IRQ");   // Internal Events
    
    adinSpi.endTransaction();
    
    Serial.println("\n... Scan erneut in 3 Sekunden ...");
    delay(3000);
}