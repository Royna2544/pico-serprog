#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "no-uart0") == 0)
    return uart_init(uart0, 115200) == 0 ? 0 : 4;
  if (argc == 2 && strcmp(argv[1], "writable-off") == 0) {
    gpio_set_function(8, GPIO_FUNC_UART);
    gpio_set_function(9, GPIO_FUNC_UART);
    if (uart_init(uart1, 74880) != 74880) return 5;
    return uart_is_writable(uart1) ? 0 : 6;
  }
  gpio_set_function(8, GPIO_FUNC_UART);
  gpio_set_function(9, GPIO_FUNC_UART);
  if (uart_init(uart1, 74880) != 74880) return 1;
  const uint8_t tx[] = {0, 10, 255};
  uint8_t rx[sizeof(tx)] = {0};
  if (uart_write_blocking(uart1, tx, sizeof(tx)) != sizeof(tx)) return 2;
  for (size_t i = 0; i < sizeof(tx); i++) {
    int tries = 2000;
    while (!uart_is_readable(uart1) && --tries) usleep(1000);
    if (!tries || uart_read_blocking(uart1, &rx[i], 1) != 1 || rx[i] != tx[i]) {
      fprintf(stderr, "UART1 byte %zu: got %u, expected %u\n", i, rx[i], tx[i]);
      return 3;
    }
  }
  uart_deinit(uart1);
  return 0;
}
