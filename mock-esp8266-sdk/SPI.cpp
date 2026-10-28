#include "SPI.h"
// SPI
SPIClass SPI;

SPIClass::SPIClass() {}
bool SPIClass::pins(int8_t sck, int8_t miso, int8_t mosi, int8_t ss) {}
void SPIClass::begin() {}
void SPIClass::end() {}

void SPIClass::setHwCs(bool use) {}
void SPIClass::setBitOrder(uint8_t bitOrder) {}
void SPIClass::setDataMode(uint8_t dataMode) {}
void SPIClass::setFrequency(uint32_t freq) {}
void SPIClass::setClockDivider(uint32_t clockDiv) {}

void SPIClass::beginTransaction(SPISettings settings) {}
void SPIClass::endTransaction() {}

uint8_t SPIClass::transfer(uint8_t data) {}
uint16_t SPIClass::transfer16(uint16_t data) {}
void SPIClass::transfer(void *buf, uint16_t count) {}

void SPIClass::write(uint8_t data) {}
void SPIClass::write16(uint16_t data) {}
void SPIClass::write16(uint16_t data, bool msb) {}
void SPIClass::write32(uint32_t data) {}
void SPIClass::write32(uint32_t data, bool msb) {}
void SPIClass::writeBytes(const uint8_t *data, uint32_t size) {}
void SPIClass::writePattern(const uint8_t *data, uint8_t size,
                            uint32_t repeat) {}
void SPIClass::transferBytes(const uint8_t *out, uint8_t *in, uint32_t size) {}