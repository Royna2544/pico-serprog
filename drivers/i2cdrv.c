#include "i2cdrv.h"

#include "core/devicetree.h"
#include <errno.h>
#include <hardware/i2c.h>

struct i2c_platform_data {
  unsigned int frequency;
  void *device_hw;
  bool slave_mode;
  uint8_t slave_address;
};

static int i2cdrv_controller_probe(struct device *dev) {
  struct i2c_platform_data *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(dev, "Failed to allocate platform data for I2C controller");
    return -ENOMEM;
  }

  if (get_prop_uint32_default(dev, "initial-clock-frequency", &pdata->frequency,
                              100000)) {
    dev_err(
        dev,
        "Failed to get initial-clock-frequency property for I2C controller");
    free(pdata);
    return -EINVAL;
  }

  if (get_prop_bool_default(dev, "slave-mode", &pdata->slave_mode, false)) {
    dev_err(dev, "Failed to get slave-mode property for I2C controller");
    free(pdata);
    return -EINVAL;
  }

  dev_dbg(dev, "I2C controller frequency: %u Hz, slave mode: %s",
          pdata->frequency, pdata->slave_mode ? "enabled" : "disabled");

  struct reg reg_info;
  if (get_reg(dev, NULL, &reg_info)) {
    dev_err(dev, "Failed to get reg property for I2C controller");
    free(pdata);
    return -EINVAL;
  }
  int base = reg_info.base;

  switch (base) {
  case I2C0_BASE:
    pdata->device_hw = i2c0;
    break;
  case I2C1_BASE:
    pdata->device_hw = i2c1;
    break;
  default:
    dev_err(dev, "Unknown I2C controller: 0x%08x", base);
    free(pdata);
    return -ENODEV;
  }

  if (i2c_init(pdata->device_hw, pdata->frequency) < 0) {
    dev_err(dev, "Failed to initialize I2C controller");
    free(pdata);
    return -EIO;
  }
  dev->platform_data = pdata;

  return 0;
}

static void i2cdrv_controller_remove(struct device *dev) {
  struct i2c_platform_data *pdata = dev->platform_data;
  if (pdata) {
    i2c_deinit(pdata->device_hw);
    free(pdata);
    dev->platform_data = NULL;
  }
}

static int i2cdrv_controller_set_frequency(const struct device *dev,
                                           unsigned int frequency) {
  struct i2c_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  pdata->frequency = frequency;
  i2c_set_baudrate(pdata->device_hw, frequency);
  dev_dbg(dev, "I2C controller frequency set to %u Hz", frequency);
  return 0;
}

static int i2cdrv_controller_get_frequency(const struct device *dev,
                                           unsigned int *frequency) {
  struct i2c_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  if (!frequency) {
    dev_err(dev, "Frequency pointer is NULL");
    return -EINVAL;
  }
  *frequency = pdata->frequency;
  return 0;
}

static int i2cdrv_controller_set_slave_mode(const struct device *dev,
                                            bool enable,
                                            uint8_t slave_address) {
  struct i2c_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  pdata->slave_mode = enable;
  pdata->slave_address = slave_address;
  i2c_set_slave_mode(pdata->device_hw, enable, slave_address);
  dev_dbg(dev, "I2C controller slave mode %s, address: 0x%02x",
          enable ? "enabled" : "disabled", slave_address);
  return 0;
}

static int i2cdrv_controller_is_slave_mode(const struct device *dev,
                                           bool *is_slave_mode,
                                           uint8_t *slave_address) {
  struct i2c_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  if (!is_slave_mode || !slave_address) {
    dev_err(dev, "Output pointers are NULL");
    return -EINVAL;
  }
  *is_slave_mode = pdata->slave_mode;
  if (pdata->slave_mode)
    *slave_address = pdata->slave_address;
  return 0;
}

static int i2cdrv_controller_read(const struct device *dev, uint8_t address,
                                  uint8_t *data, size_t len) {
  struct i2c_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  int ret = i2c_read_blocking(pdata->device_hw, address, data, len, false);
  if (ret < 0) {
    dev_err(dev, "I2C read failed with error: %d", ret);
    return ret;
  }
  return ret; // Return the number of bytes read
}

static int i2cdrv_controller_write(const struct device *dev, uint8_t address,
                                   const uint8_t *data, size_t len) {
  struct i2c_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  int ret = i2c_write_blocking(pdata->device_hw, address, data, len, false);
  if (ret < 0) {
    dev_err(dev, "I2C write failed with error: %d", ret);
    return ret;
  }
  return ret; // Return the number of bytes written
}

static const struct driver_fdt_match_id pico_i2c_match_table[] = {
    {.compatible = "raspberrypi,rp2040-i2c-controller", .data = 0}, {}};

struct i2c_controller_driver rp2040_i2c_controller_driver = {
    .drv =
        {
            .name = "rp2040-i2c",
            .probe = i2cdrv_controller_probe,
            .remove = i2cdrv_controller_remove,
            .match_table = pico_i2c_match_table,
        },
    .set_frequency = i2cdrv_controller_set_frequency,
    .get_frequency = i2cdrv_controller_get_frequency,
    .set_slave_mode = i2cdrv_controller_set_slave_mode,
    .is_slave_mode = i2cdrv_controller_is_slave_mode,
    .read = i2cdrv_controller_read,
    .write = i2cdrv_controller_write,
};

PLATFORM_DRIVER(rp2040_i2c_controller_driver);

int i2c_controller_set_frequency(const struct device *dev,
                                 unsigned int frequency) {
  struct i2c_controller_driver *i2c_controller_drv =
      container_of(dev->drv, struct i2c_controller_driver, drv);
  if (!i2c_controller_drv->set_frequency) {
    dev_err(dev, "I2C controller driver does not support set_frequency");
    return -ENOSYS;
  }
  return i2c_controller_drv->set_frequency(dev, frequency);
}

int i2c_controller_get_frequency(const struct device *dev,
                                 unsigned int *frequency) {
  struct i2c_controller_driver *i2c_controller_drv =
      container_of(dev->drv, struct i2c_controller_driver, drv);
  if (!i2c_controller_drv->get_frequency) {
    dev_err(dev, "I2C controller driver does not support get_frequency");
    return -ENOSYS;
  }
  return i2c_controller_drv->get_frequency(dev, frequency);
}

int i2c_controller_set_slave_mode(const struct device *dev, bool enable,
                                  uint8_t slave_address) {
  struct i2c_controller_driver *i2c_controller_drv =
      container_of(dev->drv, struct i2c_controller_driver, drv);
  if (!i2c_controller_drv->set_slave_mode) {
    dev_err(dev, "I2C controller driver does not support set_slave_mode");
    return -ENOSYS;
  }
  return i2c_controller_drv->set_slave_mode(dev, enable, slave_address);
}

int i2c_controller_is_slave_mode(const struct device *dev, bool *is_slave_mode,
                                 uint8_t *slave_address) {
  struct i2c_controller_driver *i2c_controller_drv =
      container_of(dev->drv, struct i2c_controller_driver, drv);
  if (!i2c_controller_drv->is_slave_mode) {
    dev_err(dev, "I2C controller driver does not support is_slave_mode");
    return -ENOSYS;
  }
  return i2c_controller_drv->is_slave_mode(dev, is_slave_mode, slave_address);
}

int i2c_device_read(const struct device *dev, uint8_t *data, size_t len) {
  struct i2c_device_driver *i2c_device_drv =
      container_of(dev->drv, struct i2c_device_driver, drv);
  if (!i2c_device_drv->read) {
    dev_err(dev, "I2C device driver does not support read");
    return -ENOSYS;
  }
  return i2c_device_drv->read(dev, data, len);
}

int i2c_device_write(const struct device *dev, const uint8_t *data,
                     size_t len) {
  struct i2c_device_driver *i2c_device_drv =
      container_of(dev->drv, struct i2c_device_driver, drv);
  if (!i2c_device_drv->write) {
    dev_err(dev, "I2C device driver does not support write");
    return -ENOSYS;
  }
  return i2c_device_drv->write(dev, data, len);
}

struct i2c_device_pdata {
  uint8_t address;
};

static int i2cdrv_device_probe(struct device *dev) {
  struct i2c_controller_driver *i2c_controller_drv =
      container_of(dev->parent->drv, struct i2c_controller_driver, drv);

  if (!i2c_controller_drv->is_slave_mode) {
    dev_warn(dev, "I2C controller driver does not support is_slave_mode");
  } else {
    bool is_slave_mode;
    uint8_t slave_address;
    if (i2c_controller_drv->is_slave_mode(dev->parent, &is_slave_mode,
                                          &slave_address) == 0 &&
        is_slave_mode) {
      dev_err(dev, "Parent I2C controller is in slave mode");
      return -ENODEV;
    }
  }

  struct reg reg_info;
  if (get_reg(dev, NULL, &reg_info)) {
    dev_err(dev, "Failed to get reg property for I2C device");
    return -EINVAL;
  }

  struct i2c_device_pdata *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(dev, "Failed to allocate platform data for I2C device");
    return -ENOMEM;
  }
  pdata->address = reg_info.base;
  dev->platform_data = pdata;
  dev_dbg(dev, "I2C device address: 0x%02x", pdata->address);

  return 0;
}

static void i2cdrv_device_remove(struct device *dev) {
  struct i2c_device_pdata *pdata = dev->platform_data;
  if (pdata) {
    free(pdata);
    dev->platform_data = NULL;
  }
}

static int i2cdrv_device_read(const struct device *dev, uint8_t *data,
                              size_t len) {
  struct i2c_controller_driver *i2c_controller_drv =
      container_of(dev->parent->drv, struct i2c_controller_driver, drv);
  if (!i2c_controller_drv->read) {
    dev_err(dev, "I2C controller driver does not support read");
    return -ENOSYS;
  }
  struct i2c_device_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  return i2c_controller_drv->read(dev->parent, pdata->address, data, len);
}

static int i2cdrv_device_write(const struct device *dev, const uint8_t *data,
                               size_t len) {
  struct i2c_controller_driver *i2c_controller_drv =
      container_of(dev->parent->drv, struct i2c_controller_driver, drv);
  if (!i2c_controller_drv->write) {
    dev_err(dev, "I2C controller driver does not support write");
    return -ENOSYS;
  }
  struct i2c_device_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  return i2c_controller_drv->write(dev->parent, pdata->address, data, len);
}

static const struct driver_fdt_match_id pico_i2c_device_match_table[] = {
    {.compatible = "raspberrypi,rp2040-i2c-device", .data = 0}, {}};

static struct i2c_device_driver rp2040_i2c_device_driver = {
    .drv =
        {
            .name = "rp2040-i2c-device",
            .probe = i2cdrv_device_probe,
            .remove = i2cdrv_device_remove,
            .match_table = pico_i2c_device_match_table,
        },
    .read = i2cdrv_device_read,
    .write = i2cdrv_device_write,
};
PLATFORM_DRIVER(rp2040_i2c_device_driver);