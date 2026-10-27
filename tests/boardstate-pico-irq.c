#include "boardstate-daemon/client.h"
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <pico/stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_int calls;
static atomic_int events_seen;
static atomic_int pin_seen;

static void bank_handler(void) {
  uint32_t events = gpio_get_irq_event_mask(20);
  if (events & GPIO_IRQ_EDGE_FALL) {
    atomic_store(&events_seen, events);
    gpio_acknowledge_irq(20, GPIO_IRQ_EDGE_FALL);
    atomic_fetch_add(&calls, 1);
  }
}

static void default_handler(uint gpio, uint32_t events) {
  atomic_store(&pin_seen, gpio);
  atomic_store(&events_seen, events);
  atomic_fetch_add(&calls, 1);
}

static int drive_esp(bool low) {
  if (board_gpio("esp", 0, "DIR", "0") < 0)
    return -1;
  if (board_gpio("esp", 0, "PUT", low ? "0" : "1") < 0)
    return -1;
  return board_gpio("esp", 0, "DIR", "1");
}

int main(int argc, char **argv) {
  if (argc != 2 || (strcmp(argv[1], "raw") && strcmp(argv[1], "default")))
    return 2;
  bool use_default = strcmp(argv[1], "default") == 0;
  if (use_default)
    gpio_set_irq_enabled_with_callback(20, GPIO_IRQ_EDGE_FALL, true, default_handler);
  else {
    irq_set_exclusive_handler(IO_IRQ_BANK0, bank_handler);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_set_irq_enabled(20, GPIO_IRQ_EDGE_FALL, true);
  }
  if (drive_esp(false) < 0 || drive_esp(true) < 0)
    return 2;
  for (int i = 0; i < 500 && atomic_load(&calls) == 0; ++i)
    sleep_ms(1);
  if (atomic_load(&calls) != 1 ||
      atomic_load(&events_seen) != GPIO_IRQ_EDGE_FALL ||
      (use_default && atomic_load(&pin_seen) != 20) ||
      gpio_get_irq_event_mask(20) != 0) {
    fprintf(stderr, "IRQ %s: calls=%d events=%d pin=%d pending=%u\n",
            argv[1], atomic_load(&calls), atomic_load(&events_seen),
            atomic_load(&pin_seen), gpio_get_irq_event_mask(20));
    return 1;
  }
  gpio_set_irq_enabled(20, GPIO_IRQ_EDGE_FALL, false);
  if (drive_esp(false) < 0 || drive_esp(true) < 0)
    return 2;
  sleep_ms(20);
  if (atomic_load(&calls) != 1 || gpio_get_irq_event_mask(20) != 0)
    return 1;
  if (board_gpio("esp", 0, "DIR", "0") < 0)
    return 2;
  irq_set_enabled(IO_IRQ_BANK0, false);
  return 0;
}
