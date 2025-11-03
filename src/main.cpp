#include <Arduino.h>

// === Konfiguration ===
const int pwmInputPin  = 25;  // Eingangspin für PWM-Signal
const int pwmOutputPin = 26;  // Ausgangspin für PWM-Erzeugung

// Für die PWM-Ausgabe
const int pwmChannel   = 0;
const int pwmResolution = 8;  // 8 Bit = Wertebereich 0–255
float pwmOutputFreq = 2000.0; // Ausgangsfrequenz in Hz (variabel)

// Für die Frequenzmessung
volatile unsigned long lastRiseTime = 0;
volatile unsigned long period = 0;
volatile bool newCycle = false;

// === Interrupt-Service-Routine ===
void IRAM_ATTR handlePwmRise() {
  unsigned long now = micros();
  period = now - lastRiseTime;
  lastRiseTime = now;
  newCycle = true;
}

// === Setup ===
void setup() {
  Serial.begin(115200);

  // Eingangspin konfigurieren und Interrupt aktivieren
  pinMode(pwmInputPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(pwmInputPin), handlePwmRise, RISING);

  // PWM-Ausgang konfigurieren
  ledcSetup(pwmChannel, pwmOutputFreq, pwmResolution);
  ledcAttachPin(pwmOutputPin, pwmChannel);
  ledcWrite(pwmChannel, 128); // 50 % Duty-Cycle (128/255)
  
  Serial.println("PWM-Frequenzmessung und -Ausgabe gestartet...");
}

// === Loop ===
void loop() {
  static unsigned long lastPrint = 0;
  static float measuredFreq = 0.0;

  if (newCycle) {
    newCycle = false;
    if (period > 0) {
      measuredFreq = 1000000.0 / period; // Hz berechnen (Mikrosekunden → Sekunden)
    }
  }

  // Beispielhafte Ausgabe alle 500 ms
  if (millis() - lastPrint > 500) {
    lastPrint = millis();
    Serial.print("Gemessene Frequenz (Eingang): ");
    Serial.print(measuredFreq, 2);
    Serial.println(" Hz");
    Serial.print("Ausgangsfrequenz (Soll): ");
    Serial.print(pwmOutputFreq, 2);
    Serial.println(" Hz\n");
  }

  // Beispiel: Frequenzänderung der Ausgabe nach 5 Sekunden
  if (millis() > 5000 && pwmOutputFreq != 1000.0) {
    pwmOutputFreq = 1000.0; // Neue Ausgangsfrequenz setzen
    ledcSetup(pwmChannel, pwmOutputFreq, pwmResolution);
    ledcAttachPin(pwmOutputPin, pwmChannel);
    ledcWrite(pwmChannel, 128);
    Serial.println("Ausgangsfrequenz geändert auf 1000 Hz");
  }
}