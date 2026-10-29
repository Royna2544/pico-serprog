#pragma once

#include "core/driver.h"
#include <stdint.h>

enum display_mode {
  DISPLAY_ON = 1,
  CURSOR_ON = 2,
  CURSOR_BLINK = 4,
};

struct pcf8574t_16x2_lcd_driver {
  struct driver drv;

  int (*clear)(const struct device *dev);
  int (*set_cursor)(const struct device *dev, uint8_t row, uint8_t col);
  int (*write_char)(const struct device *dev, char c);
  int (*write_string)(const struct device *dev, const char *str);

  int (*set_display_mode)(const struct device *dev, enum display_mode mode);
};
