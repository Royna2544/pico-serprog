#include "../core/driver.h"
#include <stdint.h>

struct led_controller_driver {
  struct driver drv;
};

struct led_controller_device {
  struct device *dev;
};

struct led_device_driver {
  struct driver drv;

  // Set the LED state (on/off)
  void (*set_state)(struct device *dev, bool state);

  // Pulse the LED for a specified duration (in milliseconds) and total time (in
  // seconds)
  void (*pulse)(struct device *dev, uint32_t duration_ms,
                uint32_t total_seconds);
};

void led_set_state(struct device *dev, bool state);
void led_pulse(struct device *dev, uint32_t duration_ms,
               uint32_t total_seconds);
