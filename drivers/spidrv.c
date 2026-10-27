#include "spidrv.h"
#include "core/devicetree.h"
#include "drivers/gpiolib.h"
#include <core/printk.h>
#include <hardware/gpio.h>
#include <hardware/spi.h>
#include <libfdt.h>
#include <string.h>
#include <sys/errno.h>

struct spi_master_platform_data {
  void *hw;
  unsigned int default_baud;
  unsigned int current_baud;
};

struct spi_chip_platform_data {
  struct gpio_desc *cs;
  unsigned int default_baud;
  unsigned int saved_baud;
  bool asserted;
};

static void spidrv_master_set_baud(const struct device *dev, uint32_t baud) {
  struct spi_master_platform_data *pdata = dev->platform_data;
  if (pdata->current_baud == baud) {
    return;
  }
  pdata->current_baud = spi_set_baudrate(pdata->hw, baud);
  dev_dbg(dev, "%s: set baud to %u: accepted %u", __func__, baud,
          pdata->current_baud);
}

static uint32_t spidrv_master_get_baud(const struct device *dev) {
  struct spi_master_platform_data *pdata = dev->platform_data;
  return pdata->current_baud = spi_get_baudrate(pdata->hw);
}

static int spidrv_master_probe(struct device *dev) {
  struct spi_master_platform_data *pdata = dev->platform_data =
      calloc(1, sizeof(struct spi_master_platform_data));
  if (!pdata) {
    dev->platform_data = NULL;
    return -ENOMEM;
  }
  int len;

  spi_inst_t *spi;

  struct reg reg_info;

  if (get_reg(dev, NULL, &reg_info)) {
    return -EINVAL;
  }

  switch (reg_info.base) {
  case SPI0_BASE:
    spi = spi0;
    break;
  case SPI1_BASE:
    spi = spi1;
    break;
  default:
    dev_err(dev, "Unknown SPI controller: 0x%08x", reg_info.base);
    return -ENODEV;
  }
  pdata->hw = spi;
  if (get_prop_uint32_default(dev, "default-baud", &pdata->default_baud,
                              1000000)) {
    dev_err(dev, "Failed to get default baud rate for device %s", dev->name);
    return -EINVAL;
  }

  // Setup PL022
  spi_init(pdata->hw, pdata->default_baud);

  return 0;
}

static int spidrv_master_read_nbytes(const struct device *dev, void *buf,
                                     uint32_t len) {
  struct spi_master_platform_data *pdata = dev->platform_data;
  return spi_read_blocking(pdata->hw, 0, buf, len);
}

static int spidrv_master_write_nbytes(const struct device *dev, const void *buf,
                                      uint32_t len) {
  struct spi_master_platform_data *pdata = dev->platform_data;
  return spi_write_blocking(pdata->hw, buf, len);
}

static void spidrv_master_remove(struct device *dev) {
  struct spi_master_platform_data *pdata = dev->platform_data;

  // Disable SPI peripheral
  spi_deinit(pdata->hw);

  free(pdata);
  dev->platform_data = NULL;
}

static int spidrv_chip_probe(struct device *dev) {
  struct spi_chip_platform_data *pdata = dev->platform_data =
      calloc(1, sizeof(struct spi_chip_platform_data));
  if (!pdata) {
    return -ENOMEM;
  }
  dev->platform_data = pdata;

  dev_info(dev, "Probing SPI chip device %s", dev->name);
  if (get_prop_uint32_default(dev, "default-baud", &pdata->default_baud,
                              1000000)) {
    dev_err(dev, "Failed to get default baud rate for device %s", dev->name);
    goto err_free;
  }
  struct gpio_desc *cs_gpio =
      gpio_request(dev, dev->fdt_node_offset, "cs-gpio");

  if (!cs_gpio) {
    goto err_free;
  }

  pdata->cs = cs_gpio;
  return 0;
err_free:
  free(pdata);
  dev->platform_data = NULL;
  return -EINVAL;
}

static void spidrv_chip_assert_cs(const struct device *dev) {
  struct spi_chip_platform_data *pdata = dev->platform_data;
  struct spi_master_platform_data *master_pdata = dev->parent->platform_data;

  if (pdata->asserted) {
    dev_err(dev, "Chip select already asserted for device %s", dev->name);
    return;
  }

  if (!master_pdata) {
    dev_err(dev, "Parent device %s has no platform data", dev->parent->name);
    return;
  }
  // Push up the baud rate to the default for this chip, and save the current
  // baud rate for later restoration.
  pdata->saved_baud = spi_get_baudrate(master_pdata->hw);
  spi_set_baudrate(master_pdata->hw, pdata->default_baud);

  // Assert the chip select GPIO
  gpio_set_value(pdata->cs, 1);
  pdata->asserted = true;
}

static void spidrv_chip_deassert_cs(const struct device *dev) {
  struct spi_chip_platform_data *pdata = dev->platform_data;
  // Restore the previous baud rate for the SPI master
  struct spi_master_platform_data *master_pdata = dev->parent->platform_data;

  if (!pdata->asserted) {
    dev_err(dev, "Chip select not asserted for device %s", dev->name);
    return;
  }

  // Deassert the chip select GPIO
  gpio_set_value(pdata->cs, 0);

  if (!master_pdata) {
    dev_err(dev, "Parent device %s has no platform data", dev->parent->name);
    return;
  }
  spi_set_baudrate(master_pdata->hw, pdata->saved_baud);
  pdata->saved_baud = 0; // Clear the saved baud rate after restoration
  pdata->asserted = false;
}

static void spidrv_chip_remove(struct device *dev) {
  struct spi_chip_platform_data *pdata = dev->platform_data;
  gpio_desc_release(dev, pdata->cs);
  free(pdata);
  dev->platform_data = NULL;
}

static const struct driver_fdt_match_id pico_spi_master_match_table[] = {
    {.compatible = "raspberrypi,rp2040-spi-master", .data = 0}, {}};
static const struct driver_fdt_match_id pico_spi_chip_match_table[] = {
    {.compatible = "raspberrypi,rp2040-spi-chip", .data = 0}, {}};

static struct spi_master_driver pico_spi_driver = {
    .drv = (struct driver){.match_table = pico_spi_master_match_table,
                           .name = "pico-spi-drv",
                           .probe = spidrv_master_probe,
                           .remove = spidrv_master_remove},
    .set_baud = spidrv_master_set_baud,
    .get_baud = spidrv_master_get_baud,
    .read_nbytes = spidrv_master_read_nbytes,
    .write_nbytes = spidrv_master_write_nbytes};

static struct spi_chip_driver pico_spi_chip_driver = {
    .drv = (struct driver){.match_table = pico_spi_chip_match_table,
                           .name = "pico-spi-chip-drv",
                           .probe = spidrv_chip_probe,
                           .remove = spidrv_chip_remove},
    .assert_cs = spidrv_chip_assert_cs,
    .deassert_cs = spidrv_chip_deassert_cs};

PLATFORM_DRIVER(pico_spi_driver);
PLATFORM_DRIVER(pico_spi_chip_driver);

int spi_master_set_baud(const struct device *dev, uint32_t baud) {
  struct spi_master_driver *drv =
      container_of(dev->drv, struct spi_master_driver, drv);
  if (!drv || !drv->set_baud) {
    dev_err(dev, "SPI master driver does not support set_baud");
    return -ENOSYS;
  }
  drv->set_baud(dev, baud);
  return 0;
}

uint32_t spi_master_get_baud(const struct device *dev) {
  struct spi_master_driver *drv =
      container_of(dev->drv, struct spi_master_driver, drv);
  if (!drv || !drv->get_baud) {
    dev_err(dev, "SPI master driver does not support get_baud");
    return -ENOSYS;
  }
  return drv->get_baud(dev);
}

int spi_master_read_nbytes(const struct device *dev, void *buf, uint32_t len) {
  struct spi_master_driver *drv =
      container_of(dev->drv, struct spi_master_driver, drv);
  if (!drv || !drv->read_nbytes) {
    dev_err(dev, "SPI chip driver does not support read_nbytes");
    return -ENOSYS;
  }
  return drv->read_nbytes(dev, buf, len);
}

int spi_master_write_nbytes(const struct device *dev, const void *buf,
                            uint32_t len) {
  struct spi_master_driver *drv =
      container_of(dev->drv, struct spi_master_driver, drv);
  if (!drv || !drv->write_nbytes) {
    dev_err(dev, "SPI chip driver does not support write_nbytes");
    return -ENOSYS;
  }
  return drv->write_nbytes(dev, buf, len);
}

int spi_chip_assert_cs(const struct device *dev) {
  struct spi_chip_driver *drv =
      container_of(dev->drv, struct spi_chip_driver, drv);
  if (!drv || !drv->assert_cs) {
    dev_err(dev, "SPI chip driver does not support assert_cs");
    return -ENOSYS;
  }
  drv->assert_cs(dev);
  return 0;
}

int spi_chip_deassert_cs(const struct device *dev) {
  struct spi_chip_driver *drv =
      container_of(dev->drv, struct spi_chip_driver, drv);
  if (!drv || !drv->deassert_cs) {
    dev_err(dev, "SPI chip driver does not support deassert_cs");
    return -ENOSYS;
  }
  drv->deassert_cs(dev);
  return 0;
}

int spi_master_write_byte(const struct device *dev, uint8_t byte) {
  return spi_master_write_nbytes(dev, &byte, 1);
}
int spi_master_read_byte(const struct device *dev, uint8_t *byte) {
  return spi_master_read_nbytes(dev, byte, 1);
}