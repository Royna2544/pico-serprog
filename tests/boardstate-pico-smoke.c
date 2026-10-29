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
  const char *tx = argc == 2 && strcmp(argv[1], "event") == 0 ?
                   ":17:EVT:JEDEC_ID\n" : ":15:SLAVE:Pico\n";
  const char *expected = argc == 2 && strcmp(argv[1], "event") == 0 ?
                         ":11:EVT:OK\n" : ":19:MASTER:ESP8266\n";
  size_t tx_len = strlen(tx), expected_len = strlen(expected);
  if (uart_write_blocking(uart1, (const uint8_t *)tx, tx_len) != tx_len) return 2;
  for (size_t i = 0; i < expected_len; i++) {
    uint8_t byte = 0;
    int tries = 2000;
    while (!uart_is_readable(uart1) && --tries) usleep(1000);
    if (!tries || uart_read_blocking(uart1, &byte, 1) != 1 ||
        byte != (uint8_t)expected[i]) {
      fprintf(stderr, "UART1 byte %zu: got %u, expected %u\n", i, byte,
              (uint8_t)expected[i]);
      return 3;
    }
  }
  uart_deinit(uart1);
  return 0;
}
