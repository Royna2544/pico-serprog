#include "core/driver.h"
#include <stdint.h>

struct i2c_device_driver {
  struct driver drv;
  int (*read)(const struct device *dev, uint8_t *data, size_t len);
  int (*write)(const struct device *dev, const uint8_t *data, size_t len);
};

struct i2c_controller_driver {
  struct driver drv;
  int (*set_frequency)(const struct device *dev, unsigned int frequency);
  int (*get_frequency)(const struct device *dev, unsigned int *frequency);
  int (*set_slave_mode)(const struct device *dev, bool enable,
                        uint8_t slave_address);
  int (*is_slave_mode)(const struct device *dev, bool *is_slave_mode,
                       uint8_t *slave_address);
  int (*read)(const struct device *dev, uint8_t address, uint8_t *data,
              size_t len);
  int (*write)(const struct device *dev, uint8_t address, const uint8_t *data,
               size_t len);
};

int i2c_controller_set_frequency(const struct device *dev,
                                 unsigned int frequency);
int i2c_controller_get_frequency(const struct device *dev,
                                 unsigned int *frequency);
int i2c_controller_set_slave_mode(const struct device *dev, bool enable,
                                  uint8_t slave_address);
int i2c_controller_is_slave_mode(const struct device *dev, bool *is_slave_mode,
                                 uint8_t *slave_address);
int i2c_device_read(const struct device *dev, uint8_t *data, size_t len);
int i2c_device_write(const struct device *dev, const uint8_t *data, size_t len);

static inline int i2c_write_byte(const struct device *dev, uint8_t data) {
  return i2c_device_write(dev, &data, 1);
}

static inline int i2c_read_byte(const struct device *dev, uint8_t *data) {
  return i2c_device_read(dev, data, 1);
}