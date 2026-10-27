#pragma once

#include <stdbool.h>

typedef unsigned int uint;
typedef void (*irq_handler_t)(void);

#define IO_IRQ_BANK0 13u

void irq_set_exclusive_handler(uint num, irq_handler_t handler);
void irq_set_enabled(uint num, bool enabled);
