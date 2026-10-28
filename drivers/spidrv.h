#ifndef __SPIDRV_H__
#define __SPIDRV_H__

#include "../core/driver.h"
#include <stdint.h>

struct spi_master_driver {
  struct driver drv;
  int (*set_baud)(const struct device *dev, uint32_t baud);
  int (*get_baud)(const struct device *dev, uint32_t *baud);
  int (*read_nbytes)(const struct device *dev, uint8_t repeating_byte, void *buf, uint32_t len);
  int (*write_nbytes)(const struct device *dev, const void *buf, uint32_t len);
};

struct spi_chip_driver {
  struct driver drv;
  int (*assert_cs)(const struct device *dev);
  int (*deassert_cs)(const struct device *dev);
  int (*set_baud)(const struct device *dev, uint32_t baud);
  int (*get_baud)(const struct device *dev, uint32_t *baud);
  int (*read_nbytes)(const struct device *dev, void *buf, uint32_t len);
  int (*write_nbytes)(const struct device *dev, const void *buf, uint32_t len);
};

struct spi_master_device {
  struct device dev;
};

int spi_chip_set_baud(const struct device *dev, uint32_t baud);
uint32_t spi_chip_get_baud(const struct device *dev);
int spi_chip_read_nbytes(const struct device *dev, void *buf, uint32_t len);
int spi_chip_write_nbytes(const struct device *dev, const void *buf,
                            uint32_t len);
int spi_chip_write_byte(const struct device *dev, uint8_t byte);
int spi_chip_read_byte(const struct device *dev, uint8_t *byte);
int spi_chip_assert_cs(const struct device *dev);
int spi_chip_deassert_cs(const struct device *dev);

#endif