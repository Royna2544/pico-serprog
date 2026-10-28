#pragma once

#include "core/driver.h"

#include <libfdt.h>
#include <stdbool.h>

typedef enum {
  GPIO_DIR_IN = 0,
  GPIO_DIR_OUT = 1,
} gpio_direction_t;

struct gpio_desc {
  struct device *master;
  int pin;
  bool active_low; // Pin is active low
  bool controlled; // Actively controlled by the driver
  bool pullup;     // Internal pull-up resistor enabled
  bool pulldown;   // Internal pull-down resistor enabled
  gpio_direction_t direction;
};

void gpio_desc_release(struct device *request_dev, struct gpio_desc *desc);
int gpio_set_value(struct gpio_desc *desc, bool value);
// Get the value of a GPIO pin, with an option to read the raw high/low state
// without considering active-low configuration
int gpio_get_value(struct gpio_desc *desc, bool *value, bool raw);
int gpio_to_irq(struct gpio_desc *desc);
void gpio_reset(struct gpio_desc *desc);
int gpio_desc_set_pullup(struct gpio_desc *desc, bool enable);
int gpio_desc_set_pulldown(struct gpio_desc *desc, bool enable);

struct gpio_driver {
  struct driver drv;

  // Enable internal pull-up resistor for the specified GPIO pin
  void (*pullup)(struct gpio_desc *desc, bool enable);
  // Enable internal pull-down resistor for the specified GPIO pin
  void (*pulldown)(struct gpio_desc *desc, bool enable);
  // Set the direction of the specified GPIO pin (input or output)
  int (*set_direction)(struct gpio_desc *desc, gpio_direction_t dir);
  // Drive the specified GPIO pin to a high or low state
  int (*set_value)(struct gpio_desc *desc, bool value);
  // Read the current value of the specified GPIO pin
  int (*get_value)(struct gpio_desc *desc, bool *value);

  // Reset the specified GPIO pin to its default state
  void (*reset)(struct gpio_desc *desc);

  // Request a GPIO pin
  struct gpio_desc *(*request)(struct device *dev, struct device *requester,
                               int len, const fdt32_t *gpio_node_value);

  void (*release)(struct device *request_dev, struct gpio_desc *desc);

  int (*request_irq)(struct device *dev, struct device *requester, int len,
                     const fdt32_t *irq_node_value);
  int (*release_irq)(struct device *dev, struct device *requester, int irq);
};
struct irq_chip;
int gpiochip_register(struct device *dev, const char *name, void *drv_data,
                      struct irq_chip *irq_chip);
int gpiochip_unregister(struct device *dev);

int gpiochip_request_check(struct device *dev, struct device *requester,
                           struct gpio_desc *pin);

int gpiochip_get_driver_data(struct device *dev, void **out_drv_data);

int gpiochip_release_pin(struct device *dev, struct device *requester,
                         struct gpio_desc *pin);