#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int uart_inst_t;

#define uart0 ((uart_inst_t *)7)
#define uart1 ((uart_inst_t *)1)

bool uart_is_enabled(void *hw);

unsigned int uart_set_baudrate(void *hw, unsigned int baud);
unsigned int uart_init(void *hw, unsigned int baud);

void uart_deinit(void *hw);

bool uart_is_readable(void *hw);
bool uart_is_writable(void *hw);

int uart_read_blocking(void *hw, uint8_t *dst, size_t len);
int uart_write_blocking(void *hw, const uint8_t *src, size_t len);

int uart_getc(void *hw);

int uart_putc(void *hw, char c);

#define UART0_BASE 0x40034000
#define UART1_BASE 0x40038000

typedef enum {
  UART_PARITY_NONE,
  UART_PARITY_EVEN,
  UART_PARITY_ODD
} uart_parity_t;
static inline void uart_set_format(uart_inst_t *uart, unsigned data_bits,
                                   unsigned stop_bits, uart_parity_t parity) {}