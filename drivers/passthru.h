#pragma once

#include <core/driver.h>

struct passthru_driver {
  struct driver drv;
  void (*loop)(struct device *dev, bool *stop);
};

void passthru_handle_loop(struct device *dev, bool *stop);