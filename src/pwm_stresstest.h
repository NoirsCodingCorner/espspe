#ifndef PWM_STRESSTEST_H
#define PWM_STRESSTEST_H

#include <Arduino.h>

// === Pin Configuration ===
extern const int pwmInputPin;   // Input pin for PWM measurement
extern const int pwmOutputPin;  // Output pin for PWM generation

// === PWM Output Configuration ===
extern const int pwmChannel;
extern const int pwmResolution;
extern float pwmOutputFreq;     // Output frequency (Hz)

// === Measurement Data ===
extern volatile unsigned long lastRiseTime;
extern volatile unsigned long period;
extern volatile bool newCycle;

// === Function Declarations ===

// Interrupt handler for measuring incoming PWM frequency
void IRAM_ATTR handlePwmRise();

// Initializes input and output PWM
void setupPwmSystem();

// Updates the measured input frequency (non-blocking)
float updateMeasuredFrequency();

// Prints current frequency data to Serial monitor
void printFrequencyStatus(float measuredFreq);

// Changes the output PWM frequency dynamically
void changeOutputFrequency(float newFreq);

#endif // PWM_STRESSTEST_H
