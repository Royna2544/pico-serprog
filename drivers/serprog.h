#pragma once

#include "core/driver.h"
#include <stdint.h>

typedef void (*spi_transfer_callback_t)(const struct device *dev,
                                        const uint8_t *send, uint32_t slen,
                                        uint8_t *reply, uint32_t rlen);
typedef void (*serprog_command_callback_t)(const struct device *dev,
                                           uint8_t cmd);
struct serprog_driver {
  struct driver drv;

  void (*handle_command)(const struct device *dev);
  void (*register_xfer_callback)(const struct device *dev,
                                 spi_transfer_callback_t callback);
  void (*register_serprog_cmd_callback)(const struct device *dev,
                                        serprog_command_callback_t callback);
};

void serprogdrv_process_commands(const struct device *dev);
void serprogdrv_set_spi_transfer_callback(const struct device *dev,
                                          spi_transfer_callback_t callback);
void serprogdrv_set_command_callback(const struct device *dev,
                                     serprog_command_callback_t callback);