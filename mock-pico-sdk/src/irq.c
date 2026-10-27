#include "hardware/gpio.h"
#include "hardware/irq.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t irq_mutex = PTHREAD_MUTEX_INITIALIZER;
static irq_handler_t bank_handler;
static gpio_irq_callback_t default_handler;
static uint8_t enabled_events[30];
static bool bank_enabled;
static bool worker_started;

uint32_t gpio_get_irq_event_mask(uint gpio) {
  if (gpio >= 30) return 0;
  int events = board_gpio("pico", gpio, "IRQ_STATUS", NULL);
  return events < 0 ? 0 : (uint32_t)events;
}

void gpio_acknowledge_irq(uint gpio, uint32_t event_mask) {
  char value[16];
  snprintf(value, sizeof(value), "%u", event_mask & 0xf);
  board_gpio("pico", gpio, "IRQ_ACK", value);
}

void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled) {
  if (gpio >= 30) return;
  char value[16];
  snprintf(value, sizeof(value), "%u", event_mask & 0xf);
  if (board_gpio("pico", gpio, enabled ? "IRQ_ENABLE" : "IRQ_DISABLE", value) < 0)
    return;
  pthread_mutex_lock(&irq_mutex);
  if (enabled) enabled_events[gpio] |= event_mask & 0xf;
  else enabled_events[gpio] &= ~(event_mask & 0xf);
  pthread_mutex_unlock(&irq_mutex);
}

void gpio_set_irq_callback(gpio_irq_callback_t callback) {
  pthread_mutex_lock(&irq_mutex);
  default_handler = callback;
  pthread_mutex_unlock(&irq_mutex);
}

static void *irq_worker(void *unused) {
  (void)unused;
  for (;;) {
    pthread_mutex_lock(&irq_mutex);
    bool enabled = bank_enabled;
    irq_handler_t bank = bank_handler;
    gpio_irq_callback_t callback = default_handler;
    bool any_pin = false;
    for (uint pin = 0; pin < 30; ++pin)
      any_pin |= enabled_events[pin] != 0;
    pthread_mutex_unlock(&irq_mutex);

    if (enabled && any_pin) {
      int pending = board_irq_pending();
      if (pending == 1) {
        if (bank) {
          bank();
        } else if (callback) {
          for (uint pin = 0; pin < 30; ++pin) {
            uint32_t events = gpio_get_irq_event_mask(pin);
            if (events) {
              gpio_acknowledge_irq(pin, events);
              callback(pin, events);
            }
          }
        }
      }
      usleep(pending < 0 ? 10000 : 1000);
    } else {
      usleep(10000);
    }
  }
  return NULL;
}

void irq_set_exclusive_handler(uint num, irq_handler_t handler) {
  if (num != IO_IRQ_BANK0) return;
  pthread_mutex_lock(&irq_mutex);
  bank_handler = handler;
  pthread_mutex_unlock(&irq_mutex);
}

void irq_set_enabled(uint num, bool enabled) {
  if (num != IO_IRQ_BANK0) return;
  pthread_mutex_lock(&irq_mutex);
  bank_enabled = enabled;
  if (enabled && !worker_started) {
    pthread_t worker;
    int rc = pthread_create(&worker, NULL, irq_worker, NULL);
    if (rc == 0) {
      worker_started = true;
      pthread_detach(worker);
    } else {
      bank_enabled = false;
      fprintf(stderr, "mock GPIO IRQ worker: %s\n", strerror(rc));
    }
  }
  pthread_mutex_unlock(&irq_mutex);
}

void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask,
                                        bool enabled, gpio_irq_callback_t callback) {
  if (enabled) gpio_set_irq_callback(callback);
  gpio_set_irq_enabled(gpio, event_mask, enabled);
  if (!enabled) gpio_set_irq_callback(callback);
  if (enabled) irq_set_enabled(IO_IRQ_BANK0, true);
}
