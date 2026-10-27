#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define spi0 (void *)0
#define spi1 (void *)1

static inline void spi_init(void *spi, uint32_t baudrate) {
  printf("spi_init: spi=%p, baudrate=%u\n", spi, baudrate);
}
static inline void spi_deinit(void *spi) {
  printf("spi_deinit: spi=%p\n", spi);
}
static inline uint32_t spi_get_baudrate(void *spi) {
  printf("spi_get_baudrate: spi=%p\n", spi);
  return 054444;
}
static inline uint32_t spi_set_baudrate(void *spi, uint32_t baudrate) {
  printf("spi_set_baudrate: spi=%p, baudrate=%u\n", spi, baudrate);
  return baudrate;
}
static inline int spi_write_blocking(void *spi, const uint8_t *src,
                                     size_t len) {
  printf("spi_write_blocking: spi=%p, src=%p, len=%zu\n", spi, src, len);
  return len;
}
static inline int spi_read_blocking(void *spi, uint8_t repeated_tx_data,
                                    uint8_t *dst, size_t len) {
  printf("spi_read_blocking: spi=%p, repeated_tx_data=%u, dst=%p, len=%zu\n",
         spi, repeated_tx_data, dst, len);
  return len;
}

typedef void *spi_inst_t;
#define SPI0_BASE 0x4003c000
#define SPI1_BASE 0x40040000