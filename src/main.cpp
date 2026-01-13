#include <Arduino.h>
#include <SPI.h>

// ==========================================
// 1. PIN KONFIGURATION
// ==========================================
#define ADIN_CS   5
#define ADIN_MOSI 23
#define ADIN_SCK  18
#define ADIN_MISO 19  
#define ADIN_INT  32
#define ADIN_RST  4

// 1 MHz: Konservativ & Sicher für Fädeldrähte/Breadboards
#define SPI_SPEED 1000000 

SPIClass adinSpi(VSPI);

const uint32_t EXPECTED_ID_1110 = 0x0283BC91;
const uint32_t EXPECTED_ID_2111 = 0x0283BCA1; // Falls es der große Bruder ist

// ==========================================
// 2. HELPER (CRC & PARITY)
// ==========================================

// Berechnet CRC8 (Polynom 0x7) für Generic SPI
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

// Zählt gesetzte Bits (für Odd Parity im OA Header)
int countSetBits(uint32_t n) {
    int count = 0;
    while (n > 0) { n &= (n - 1); count++; }
    return count;
}

// 32-Bit Transfer (MSB First) für Open Alliance
uint32_t transfer32(uint32_t data) {
    uint32_t resp = 0;
    resp |= (uint32_t)adinSpi.transfer((data >> 24) & 0xFF) << 24;
    resp |= (uint32_t)adinSpi.transfer((data >> 16) & 0xFF) << 16;
    resp |= (uint32_t)adinSpi.transfer((data >> 8) & 0xFF) << 8;
    resp |= (uint32_t)adinSpi.transfer((data >> 0) & 0xFF);
    return resp;
}

// ==========================================
// 3. PRINTING & ANALYSE
// ==========================================

void printSectionHeader(int strapLevel) {
    Serial.println();
    Serial.println("==========================================================================================");
    if (strapLevel == LOW) 
        Serial.println("[RESET] STRAP: LOW (Erwartet: OA Protected ODER Gen CRC)");
    else                    
        Serial.println("[RESET] STRAP: HIGH (Erwartet: OA Unprotected ODER Gen No-CRC)");
    Serial.println("==========================================================================================");
    Serial.printf(" %-28s | %-12s | %-12s | %-12s | %s\n", "TEST PROTOCOL", "TX CMD", "RX DATA", "RX FOOTER", "RESULT");
    Serial.println("------------------------------+--------------+--------------+--------------+----------------");
}

void printRow(const char* protoName, String txStr, uint32_t rxData, uint32_t rxFooter) {
    String statusStr;
    bool idMatch = (rxData == EXPECTED_ID_1110) || (rxData == EXPECTED_ID_2111);
    
    // Einfache Loopback Erkennung (wenn RX == TX)
    // Wir vergleichen die oberen 16 Bit, da Generic SPI Daten shiften kann
    uint32_t txVal = strtoul(txStr.substring(0, 4).c_str(), NULL, 16);
    bool loopback = ((rxData >> 16) == txVal) || (rxData == txVal);

    if (idMatch) {
        statusStr = "✅ SUCCESS (ID MATCH)";
    } else if (rxData == 0x00000000) {
        statusStr = "💤 SILENCE (0V)";
    } else if (rxData == 0xFFFFFFFF) {
        statusStr = "❌ HIGH-Z (Float)";
    } else if (loopback) {
        statusStr = "⚠️ LOOPBACK / ECHO";
    } else {
        statusStr = "❓ UNKNOWN DATA";
    }

    // Footer Anzeige (nur wenn vorhanden)
    char footerHex[16] = "-";
    if (rxFooter != 0xDEADC0DE) { 
        sprintf(footerHex, "%08X", rxFooter);
    }

    Serial.printf(" %-28s | %-12s | %08X     | %-12s | %s\n", 
                  protoName, txStr.c_str(), rxData, footerHex, statusStr.c_str());
}

// ==========================================
// 4. TEST IMPLEMENTIERUNGEN (VALIDIERT)
// ==========================================

// --- A. GENERIC SPI NO CRC (Command 0x8001) ---
// Erwartet bei Jumper=1, Strap=1
void test_Generic_NoCRC() {
    uint16_t cmd = 0x8001; // R/W=1, Addr=1
    char txBuf[16]; sprintf(txBuf, "%04X", cmd);

    digitalWrite(ADIN_CS, LOW);
    adinSpi.transfer16(cmd);
    adinSpi.transfer(0x00); // Turnaround Byte
    
    // Generic liest Daten: Wir lesen 4 Bytes
    uint32_t rx = 0;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 24;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 16;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 8;
    rx |= (uint32_t)adinSpi.transfer(0x00);
    digitalWrite(ADIN_CS, HIGH);

    printRow("Generic (NoCRC) 0x8001", String(txBuf), rx, 0xDEADC0DE);
}

// --- B. GENERIC SPI WITH CRC (Command 0x8001) ---
// Erwartet bei Jumper=1, Strap=0
void test_Generic_WithCRC() {
    uint16_t cmd = 0x8001;
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    uint8_t crc = calculateCRC8(buf, 2);
    char txBuf[16]; sprintf(txBuf, "%04X:%02X", cmd, crc);

    digitalWrite(ADIN_CS, LOW);
    adinSpi.transfer16(cmd);
    adinSpi.transfer(crc);  // CRC senden
    adinSpi.transfer(0x00); // Turnaround
    
    uint32_t rx = 0;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 24;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 16;
    rx |= (uint32_t)adinSpi.transfer(0x00) << 8;
    rx |= (uint32_t)adinSpi.transfer(0x00);
    digitalWrite(ADIN_CS, HIGH);

    printRow("Generic (CRC) 0x8001", String(txBuf), rx, 0xDEADC0DE);
}

// --- C. OPEN ALLIANCE UNPROTECTED ---
// Erwartet bei Jumper=0, Strap=1
void test_OA_Unprotected() {
    // Header Aufbau: DNC(0), HDRB(1), WNR(0), Addr(1) -> Shift 8
    uint32_t header = (1UL << 30) | (0x01UL << 8);    
    
    // Parity Calculation (Odd Parity über 32 Bit)
    if (countSetBits(header) % 2 == 0) header |= 1UL;
    
    char txBuf[16]; sprintf(txBuf, "%08X", header);

    digitalWrite(ADIN_CS, LOW);
    transfer32(header); 
    uint32_t rxData = transfer32(0x00000000); // Daten lesen
    digitalWrite(ADIN_CS, HIGH); // CS sofort hoch -> kein Footer erwartet

    printRow("OA Unprotected", String(txBuf), rxData, 0xDEADC0DE);
}

// --- D. OPEN ALLIANCE PROTECTED ---
// Erwartet bei Jumper=0, Strap=0
void test_OA_Protected() {
    uint32_t header = (1UL << 30) | (0x01UL << 8);    
    if (countSetBits(header) % 2 == 0) header |= 1UL;
    
    char txBuf[16]; sprintf(txBuf, "%08X", header);

    digitalWrite(ADIN_CS, LOW);
    transfer32(header); 
    uint32_t rxData = transfer32(0x00000000); 
    uint32_t rxFooter = transfer32(0x00000000); // Footer (Status+CRC) lesen!
    digitalWrite(ADIN_CS, HIGH);

    printRow("OA Protected", String(txBuf), rxData, rxFooter);
}

// ==========================================
// 5. MAIN
// ==========================================

void performStrapReset(int strapLevel) {
    printSectionHeader(strapLevel);
    
    // 1. Reset drücken
    digitalWrite(ADIN_RST, LOW); 
    delay(20);
    
    // 2. MISO Pin als Ausgang setzen (Soft-Strap)
    pinMode(ADIN_MISO, OUTPUT); 
    digitalWrite(ADIN_MISO, strapLevel); 
    delay(20);
    
    // 3. Reset loslassen
    digitalWrite(ADIN_RST, HIGH); 
    delay(50); // Chip liest jetzt den Strap
    
    // 4. WICHTIG: MISO sofort wieder auf Input
    pinMode(ADIN_MISO, INPUT); 
    
    // 5. Warten auf Boot
    delay(100); 
}

void setup() {
    Serial.begin(115200);
    pinMode(ADIN_CS, OUTPUT);
    pinMode(ADIN_RST, OUTPUT);
    digitalWrite(ADIN_CS, HIGH);
    
    adinSpi.begin(ADIN_SCK, ADIN_MISO, ADIN_MOSI, ADIN_CS);
    
    while(!Serial) delay(10);
    Serial.println("\n\n--- ADIN1110 FORENSIC SCAN (VALIDATED) ---");
    Serial.println("Druecken Sie ENTER zum Starten...");
    while(Serial.available() == 0) delay(100);
    while(Serial.available()) Serial.read();
}

void loop() {
    int straps[] = {LOW, HIGH};

    for(int s=0; s<2; s++) {
        performStrapReset(straps[s]);
        
        adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
        
        test_Generic_NoCRC();   delay(50);
        test_Generic_WithCRC(); delay(50);
        test_OA_Unprotected();  delay(50);
        test_OA_Protected();    delay(50);
        
        adinSpi.endTransaction();
        
        Serial.println("... Wartezeit ...");
        delay(1000);
    }

    Serial.println("\n--- Zyklus fertig. Neustart in 5s ---\n");
    delay(5000);
}