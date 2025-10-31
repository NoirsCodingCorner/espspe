#ifndef ADIN1110_H
#define ADIN1110_H

#include <Arduino.h>
#include "SPIDevice.h" // 1. We include the SPIDevice you just built

// Define some ADIN1110 register addresses (found in datasheet)
#define ADDR_CHIP_ID 0x0001
#define ADDR_MDIO_CTRL 0x0020
// ... many more registers ...

class ADIN1110 {

public:
  ADIN1110(SPIDevice& spiDevice);
  bool begin(); 
  uint16_t readRegister(uint16_t address);
  void writeRegister(uint16_t address, uint16_t data);
  int readFrame(byte* buffer, int maxLength);
  void writeFrame(const byte* buffer, int length);
  bool isLinkUp();

private:
  SPIDevice& _spi;
};

#endif