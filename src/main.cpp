#include <Arduino.h>
#include <SPI.h>

// ==========================================
// PIN DEFINITIONEN
// ==========================================
#define ADIN_CS   5
#define ADIN_MOSI 23
#define ADIN_SCK  18
#define ADIN_MISO 19  // Wird auch als Strap-Pin missbraucht!
#define ADIN_INT  32
#define ADIN_RST  4

#define SPI_SPEED 1000000 // 1 MHz - sicher und sauber

SPIClass adinSpi(VSPI);

// ==========================================
// HILFSFUNKTIONEN
// ==========================================

// Parität berechnen (für Open Alliance)
int countSetBits(uint32_t n) {
  int count = 0;
  while (n > 0) { n &= (n - 1); count++; }
  return count;
}

// 32-Bit Transfer (für Open Alliance)
uint32_t transfer32(uint32_t data) {
  uint32_t resp = 0;
  resp |= (uint32_t)adinSpi.transfer((data >> 24) & 0xFF) << 24;
  resp |= (uint32_t)adinSpi.transfer((data >> 16) & 0xFF) << 16;
  resp |= (uint32_t)adinSpi.transfer((data >> 8) & 0xFF) << 8;
  resp |= (uint32_t)adinSpi.transfer((data >> 0) & 0xFF);
  return resp;
}

// RESET MIT SOFT-STRAP
// Zwingt MISO während des Resets auf HIGH oder LOW
void performStrapReset(int strapLevel) {
  Serial.println("\n------------------------------------------------");
  Serial.print("[RESET] Zwinge MISO (CFG0) auf: ");
  Serial.println(strapLevel == LOW ? "LOW (GND)" : "HIGH (3.3V)");
  
  // 1. Reset aktivieren
  digitalWrite(ADIN_RST, LOW);
  delay(50);
  
  // 2. MISO Pin als Ausgang konfigurieren und Pegel setzen
  pinMode(ADIN_MISO, OUTPUT);
  digitalWrite(ADIN_MISO, strapLevel);
  
  // 3. Reset loslassen (Chip liest jetzt den Pegel)
  delay(10);
  digitalWrite(ADIN_RST, HIGH);
  
  // 4. Pegel kurz halten, bis Chip sicher gebootet hat
  delay(20);
  
  // 5. MISO wieder als SPI-Eingang freigeben
  pinMode(ADIN_MISO, INPUT);
  
  Serial.println("[RESET] Fertig. Warte auf PLL Lock...");
  delay(150); // Chip Zeit geben zum Aufwachen
}

// TEST 1: OPEN ALLIANCE PROTOKOLL
void tryOpenAlliance() {
  // Befehl: Read Register 0x01 (PHY ID)
  // Header: Control(0), HDRB(1), Read(0), Addr(1)
  uint32_t cmd = 0;
  cmd |= (1UL << 30);      // HDRB
  cmd |= (0x01 << 8);      // Adresse
  if (countSetBits(cmd) % 2 == 0) cmd |= 1UL; // Odd Parity

  adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
  digitalWrite(ADIN_CS, LOW);
  
  uint32_t rxHeader = transfer32(cmd);      // Sende Befehl
  uint32_t rxData   = transfer32(0x00000000); // Lese Daten
  
  digitalWrite(ADIN_CS, HIGH);
  adinSpi.endTransaction();

  Serial.print("   [OA-Proto]  TX: 0x"); Serial.print(cmd, HEX);
  Serial.print(" | RX: 0x"); Serial.print(rxData, HEX);
  
  if (rxData == 0x0283BC91) Serial.println(" -> SUCCESS! (ID OK)");
  else if (rxData == 0) Serial.println(" -> (0V / Low)");
  else Serial.println(" -> (Daten empfangen!)");
}

// TEST 2: GENERIC SPI PROTOKOLL
void tryGenericSPI() {
  // Befehl: Read Register 0x01
  // Header: Control(1), AutoInc(1), Read(0), Addr(0x1) -> 0xA001
  uint16_t cmd = 0xA001;

  adinSpi.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE0));
  digitalWrite(ADIN_CS, LOW);
  
  adinSpi.transfer16(cmd); // 16 Bit Header
  adinSpi.transfer(0x00);  // 8 Bit Turnaround
  
  // Wir lesen 32 Bit Daten (auch wenn Generic oft nur 16 sendet, wir lesen sicherheitshalber mehr)
  uint32_t rxData = 0;
  rxData |= (uint32_t)adinSpi.transfer(0x00) << 24;
  rxData |= (uint32_t)adinSpi.transfer(0x00) << 16;
  rxData |= (uint32_t)adinSpi.transfer(0x00) << 8;
  rxData |= (uint32_t)adinSpi.transfer(0x00);
  
  digitalWrite(ADIN_CS, HIGH);
  adinSpi.endTransaction();

  Serial.print("   [GEN-Proto] TX: 0xA001     | RX: 0x"); Serial.print(rxData, HEX);
  
  // Generic SPI sendet ID oft Byte-weise gedreht oder verschoben, 
  // wir achten einfach drauf, ob es NICHT 0 ist.
  if (rxData == 0x0283BC91) Serial.println(" -> SUCCESS! (ID OK)");
  else if (rxData == 0) Serial.println(" -> (0V / Low)");
  else Serial.println(" -> (Daten empfangen!)");
}

// ==========================================
// MAIN SETUP & LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  
  pinMode(ADIN_CS, OUTPUT);
  pinMode(ADIN_RST, OUTPUT);
  pinMode(ADIN_INT, INPUT_PULLUP);
  digitalWrite(ADIN_CS, HIGH);

  adinSpi.begin(ADIN_SCK, ADIN_MISO, ADIN_MOSI, ADIN_CS);

  while(!Serial) delay(10);
  Serial.println("\n\n################################################");
  Serial.println("   ADIN1110 PROTOCOL BRUTE FORCER");
  Serial.println("   Testet alle 4 SPI-Modi zyklisch durch.");
  Serial.println("################################################");
  delay(1000);
}

void loop() {
  // ============================================================
  // PHASE A: MISO (CFG0) auf LOW zwingen
  // ============================================================
  // Wenn Jumper CFG1 = LOW  -> Modus (0,0) = Open Alliance Protected
  // Wenn Jumper CFG1 = HIGH -> Modus (1,0) = Generic SPI with CRC
  
  performStrapReset(LOW); 
  
  Serial.println(">>> Teste Kommunikation (Strap=LOW)...");
  for(int i=0; i<3; i++) { // Wir probieren es 3x
     tryOpenAlliance();
     delay(10);
     tryGenericSPI();
     delay(100);
  }

  delay(1000); // Kurze Pause zum Lesen

  // ============================================================
  // PHASE B: MISO (CFG0) auf HIGH zwingen
  // ============================================================
  // Wenn Jumper CFG1 = LOW  -> Modus (0,1) = Open Alliance Unprotected
  // Wenn Jumper CFG1 = HIGH -> Modus (1,1) = Generic SPI without CRC
  
  performStrapReset(HIGH);
  
  Serial.println(">>> Teste Kommunikation (Strap=HIGH)...");
  for(int i=0; i<3; i++) { // Wir probieren es 3x
     tryOpenAlliance();
     delay(10);
     tryGenericSPI();
     delay(100);
  }

  Serial.println("\n... Zyklus Ende. Warte 2 Sekunden ...");
  delay(2000);
}