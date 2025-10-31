#include "ADIN1110.h"

// --- Constants for SPI Commands (from ADIN1110 Datasheet) ---
// The high bit (bit 15) of the 16-bit command is '1' for a READ
const uint16_t SPI_CMD_READ_MASK = 0x8000;
// The high bit is '0' for a WRITE
const uint16_t SPI_CMD_WRITE_MASK = 0x0000;


// Constructor: It just stores a reference to the SPIDevice
ADIN1110::ADIN1110(SPIDevice& spiDevice) : _spi(spiDevice) {
  // This syntax (": _spi(spiDevice)") is an "initializer list"
  // It's the proper C++ way to set the _spi reference
}


/**
 * @brief Reads a 16-bit value from a register.
 * @param address The 16-bit register address.
 * @return The 16-bit data read from the register.
 */
uint16_t ADIN1110::readRegister(uint16_t address) {

  // 1. Create the 16-bit READ command
  //    (Example: 0x8000 | 0x0001 = 0x8001 for CHIP_ID)
  uint16_t command = address | SPI_CMD_READ_MASK;

  // Split the command into two 8-bit bytes
  byte cmd_high = (command >> 8) & 0xFF;
  byte cmd_low = command & 0xFF;

  byte data_high = 0;
  byte data_low = 0;

  // Start the SPI bus transaction
  _spi.beginTransaction();

  // Select this chip
  _spi.select();

  // Send the 16-bit command
  _spi.transfer(cmd_high);
  _spi.transfer(cmd_low);

  // Read the 16-bit response by sending dummy bytes
  data_high = _spi.transfer(0x00);
  data_low = _spi.transfer(0x00);

  // Deselect the chip
  _spi.deselect();

  // End the transaction to release the SPI bus
  _spi.endTransaction();

  // Combine the two 8-bit bytes into a 16-bit return value
  uint16_t result = (data_high << 8) | data_low;
  return result;
}


/**
 * @brief Writes a 16-bit value to a register.
 * @param address The 16-bit register address.
 * @param data The 16-bit data to write.
 */
void ADIN1110::writeRegister(uint16_t address, uint16_t data) {
  
  // 1. Create the 16-bit WRITE command
  //    (Example: 0x0000 | 0x0020 = 0x0020 for MDIO_CTRL)
  uint16_t command = address | SPI_CMD_WRITE_MASK; // (The mask is 0x00, but this is clearer)

  // Split command and data into 8-bit bytes
  byte cmd_high = (command >> 8) & 0xFF;
  byte cmd_low = command & 0xFF;

  byte data_high = (data >> 8) & 0xFF;
  byte data_low = data & 0xFF;

  // Start the SPI bus transaction
  _spi.beginTransaction();

  // Select this chip
  _spi.select();

  // Send the 16-bit command
  _spi.transfer(cmd_high);
  _spi.transfer(cmd_low);

  // Send the 16-bit data
  _spi.transfer(data_high);
  _spi.transfer(data_low);

  // Deselect the chip
  _spi.deselect();

  // End the transaction to release the SPI bus
  _spi.endTransaction();
}


// --- Functions to be implemented next ---

/**
 * @brief Initializes the ADIN1110.
 * This is where you'd do the "Hello, Chip" test
 * and configure the chip (e.g., set Master/Slave).
 */
bool ADIN1110::begin() {
  // 1. Test communication by reading the chip ID
  uint16_t chipID = readRegister(ADDR_CHIP_ID);
  
  if (chipID != 0x1110) {
    return false; // Failed to find chip
  }

  // 2. TODO: Add initial chip configuration here
  //    - Perform a software reset
  //    - Set PHY to Master or Slave mode (required for 10BASE-T1L)
  //    - Enable interrupts
  //    - etc.

  return true; // Success
}


/**
 * @brief Reads an Ethernet frame from the RX FIFO.
 * @param buffer A pointer to a buffer to store the frame.
 * @param maxLength The max size of the buffer.
 * @return The length of the frame read, or 0 if no frame.
 */
int ADIN1110::readFrame(byte* buffer, int maxLength) {
  // TODO: Implement frame read logic
  // 1. Check RX_RDY interrupt or RX_FRAMES_IN_FIFO register
  // 2. Read the frame length from the RX FIFO
  // 3. Read the frame data from the RX FIFO (using a burst SPI read)
  // 4. Return the frame length
  return 0; // Placeholder
}


/**
 * @brief Writes an Ethernet frame to the TX FIFO.
 * @param buffer A pointer to the frame data.
 * @param length The length of the frame.
 */
void ADIN1110::writeFrame(const byte* buffer, int length) {
  // TODO: Implement frame write logic
  // 1. Write the frame length to the TX FIFO
  // 2. Write the frame data to the TX FIFO (using a burst SPI write)
  // 3. Set the TX_KICK (or similar) register bit to tell the chip to send
}


/**
 * @brief Checks if the 10BASE-T1L link is active.
 * @return true if link is up, false otherwise.
 */
bool ADIN1110::isLinkUp() {
  // TODO: Implement link status check
  // 1. Read the PHY_STATUS or similar register
  // 2. Check the "Link Up" bit
  // 3. Return the status
  return false; // Placeholder
}