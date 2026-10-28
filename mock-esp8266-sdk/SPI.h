#pragma once
#include "Arduino.h"
#include <stdint.h>

#define SPI_HAS_TRANSACTION 1

#define SPI_CLOCK_DIV2 0x00101001
#define SPI_CLOCK_DIV4 0x00241001
#define SPI_CLOCK_DIV8 0x004c1001
#define SPI_CLOCK_DIV16 0x009c1001
#define SPI_CLOCK_DIV32 0x013c1001
#define SPI_CLOCK_DIV64 0x027c1001
#define SPI_CLOCK_DIV128 0x04fc1001

constexpr uint8_t SPI_MODE0 = 0x00;
constexpr uint8_t SPI_MODE1 = 0x01;
constexpr uint8_t SPI_MODE2 = 0x10;
constexpr uint8_t SPI_MODE3 = 0x11;

class SPISettings {
public:
  SPISettings() : _clock(1000000), _bitOrder(LSBFIRST), _dataMode(SPI_MODE0) {}

  SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
      : _clock(clock), _bitOrder(bitOrder), _dataMode(dataMode) {}

  uint32_t _clock;
  uint8_t _bitOrder;
  uint8_t _dataMode;
};

class SPIClass {
public:
  SPIClass();

  bool pins(int8_t sck, int8_t miso, int8_t mosi, int8_t ss);

  void begin();
  void end();

  void setHwCs(bool use);
  void setBitOrder(uint8_t bitOrder);
  void setDataMode(uint8_t dataMode);
  void setFrequency(uint32_t freq);
  void setClockDivider(uint32_t clockDiv);

  void beginTransaction(SPISettings settings);
  void endTransaction();

  uint8_t transfer(uint8_t data);
  uint16_t transfer16(uint16_t data);
  void transfer(void *buf, uint16_t count);

  void write(uint8_t data);
  void write16(uint16_t data);
  void write16(uint16_t data, bool msb);
  void write32(uint32_t data);
  void write32(uint32_t data, bool msb);

  void writeBytes(const uint8_t *data, uint32_t size);
  void writePattern(const uint8_t *data, uint8_t size, uint32_t repeat);

  void transferBytes(const uint8_t *out, uint8_t *in, uint32_t size);
};

extern SPIClass SPI;
