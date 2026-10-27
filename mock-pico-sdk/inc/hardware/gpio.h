#pragma once

#include "boardstate-daemon/client.h"
#include "hardware/irq.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned int uint;

#define GPIO_FUNC_UART 2
#define GPIO_FUNC_SPI 3
#define GPIO_FUNC_I2C 4
#define GPIO_FUNC_PWM 5
#define GPIO_FUNC_SIO 125
#define GPIO_FUNC_CLK 6

#define GPIO_OUT true
#define GPIO_IN false

static inline void gpio_init(unsigned int gpio) {
  board_gpio("pico", gpio, "MUX", "sio");
}
static inline void gpio_deinit(unsigned int gpio) {
  board_gpio("pico", gpio, "DIR", "0");
}
static inline void gpio_set_function(unsigned int gpio, unsigned int function) {
  const char *func_str = "sio";
  switch (function) {
  case GPIO_FUNC_UART:
    func_str = "uart";
    break;
  case GPIO_FUNC_SPI:
    func_str = "spi";
    break;
  case GPIO_FUNC_I2C:
    func_str = "i2c";
    break;
  case GPIO_FUNC_PWM:
    func_str = "pwm";
    break;
  case GPIO_FUNC_SIO:
    func_str = "sio";
    break;
  case GPIO_FUNC_CLK:
    func_str = "clk";
    break;
  default:
    func_str = "sio";
    break;
  }
  board_gpio("pico", gpio, "MUX", func_str);
}
static inline void gpio_set_dir(unsigned int gpio, bool out) {
  board_gpio("pico", gpio, "DIR", out ? "1" : "0");
}
static inline void gpio_put(unsigned int gpio, bool value) {
  board_gpio("pico", gpio, "PUT", value ? "1" : "0");
}
static inline bool gpio_get(unsigned int gpio) {
  return board_gpio("pico", gpio, "GET", NULL) == 1;
}
static inline void gpio_set_pulls(unsigned int gpio, bool up, bool down) {
  char pulls[] = {up ? '1' : '0', down ? '1' : '0', 0};
  board_gpio("pico", gpio, "PULL", pulls);
}

enum gpio_irq_level {
  GPIO_IRQ_LEVEL_LOW = 0x1u,  ///< IRQ when the GPIO pin is a logical 0
  GPIO_IRQ_LEVEL_HIGH = 0x2u, ///< IRQ when the GPIO pin is a logical 1
  GPIO_IRQ_EDGE_FALL = 0x4u,  ///< IRQ when the GPIO has transitioned from a
                              ///< logical 1 to a logical 0
  GPIO_IRQ_EDGE_RISE = 0x8u,  ///< IRQ when the GPIO has transitioned from a
                              ///< logical 0 to a logical 1
};

/*! Callback function type for GPIO events
 *  \ingroup hardware_gpio
 *
 * \param gpio Which GPIO caused this interrupt
 * \param event_mask Which events caused this interrupt. See \ref gpio_irq_level
 * for details.
 * \sa gpio_set_irq_enabled_with_callback()
 * \sa gpio_set_irq_callback()
 */
typedef void (*gpio_irq_callback_t)(uint gpio, uint32_t event_mask);

uint32_t gpio_get_irq_event_mask(uint gpio);
void gpio_acknowledge_irq(uint gpio, uint32_t event_mask);
void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled);
void gpio_set_irq_callback(gpio_irq_callback_t callback);
void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask,
                                        bool enabled, gpio_irq_callback_t callback);

static inline void gpio_disable_pulls(uint gpio) {
  gpio_set_pulls(gpio, false, false);
}
