#pragma once

#include "core/driver.h"

struct serprog_driver {
  struct driver drv;

  void (*handle_command)(const struct device *dev);
};

void serprogdrv_process_commands(const struct device *dev);
