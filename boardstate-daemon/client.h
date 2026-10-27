#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uses BOARDSTATE_SOCKET, default /tmp/boardstate-daemon.sock.
 * Returns -1 and sets errno on connection/protocol failure. */
int board_gpio(const char *board, unsigned pin, const char *operation,
               const char *value);
int board_uart(const char *board, const char *operation, int byte);
int board_irq_pending(void);
int board_reset_pico(void);
int board_power(void);
int board_boot_generation(void);

#ifdef __cplusplus
}
#endif
