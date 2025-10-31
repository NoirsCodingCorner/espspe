#ifndef SPI_DEVICE_H
#define SPI_DEVICE_H

#include <Arduino.h>
#include <SPI.h> // We need the Arduino SPI library

class SPIDevice {

public:
  // --- Public Functions ---

  // Constructor: Call this to create a device.
  // It requires the CS pin and all SPI settings.
  SPIDevice(int csPin, uint32_t clockSpeed, byte bitOrder, byte dataMode);

  // Call this in setup() to initialize the hardware
  void begin();

  // Call these to wrap your SPI communication
  void beginTransaction();
  void endTransaction();

  // Manual control over the chip select (CS) pin
  void select();
  void deselect();

  // --- Data Transfer Functions ---
  // These must be called *after* beginTransaction() and select()

  // Transfers a single byte out and returns the byte that came in
  byte transfer(byte data);

  // Reads a number of bytes into a buffer (sends dummy 0x00 bytes)
  void readBuffer(byte* buffer, int length);

  // Writes a number of bytes from a buffer (ignores incoming bytes)
  void writeBuffer(const byte* buffer, int length);

private:
  // --- Private Variables ---

  // Stores the CS pin number
  int _csPin;
  
  // Stores the SPI settings object for this specific device
  SPISettings _settings;
};

#endif