#include "SPIDevice.h"

SPIDevice::SPIDevice(int csPin, uint32_t clockSpeed, byte bitOrder, byte dataMode) {
  _csPin = csPin;
  _settings = SPISettings(clockSpeed, bitOrder, dataMode);
}

void SPIDevice::begin() {
  pinMode(_csPin, OUTPUT);
  deselect(); // Make sure the chip is not selected by default
    SPI.begin();
}


void SPIDevice::beginTransaction() {
  SPI.beginTransaction(_settings);
}

void SPIDevice::endTransaction() {
  SPI.endTransaction();
}


void SPIDevice::select() {
  digitalWrite(_csPin, LOW);
}

void SPIDevice::deselect() {
  digitalWrite(_csPin, HIGH);
}



byte SPIDevice::transfer(byte data) {
  return SPI.transfer(data);
}

void SPIDevice::readBuffer(byte* buffer, int length) {
  int i = 0;
  for (i = 0; i < length; i = i + 1) {
    buffer[i] = SPI.transfer(0x00); // Send a dummy byte (0x00) to clock in data
  }
}

void SPIDevice::writeBuffer(const byte* buffer, int length) {
  int i = 0;
  // Loop 'length' times
  for (i = 0; i < length; i = i + 1) {
    SPI.transfer(buffer[i]); // Send the byte
  }
}