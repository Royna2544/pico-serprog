#pragma once

#include "core/driver.h"
#include "gpiolib.h"

struct pinctrl_driver {
  struct driver drv;

  // Request a pinctrl state by name
  int (*request_state)(struct device *dev, const struct device *requester,
                       const char *state_name);
  // Release the currently requested pinctrl state
  int (*release_state)(struct device *dev, const struct device *requester,
                       const char *state_name);

  // Externally, handle request to turn a pin into GPIO function.
  int (*pinctrl_gpio_request)(struct device *dev,
                              const struct device *requester,
                              struct gpio_desc *pin);
  // Externally, handle request to release a pin from GPIO function.
  int (*pinctrl_gpio_release)(struct device *dev,
                              const struct device *requester,
                              struct gpio_desc *pin);
};

struct pinctrl_device {
  struct device dev;
};