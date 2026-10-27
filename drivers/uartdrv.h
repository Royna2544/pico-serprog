#include "../core/driver.h"
#include <stddef.h>
#include <stdint.h>

typedef struct uart_line_coding {
  uint8_t stop_bits; ///< 0: 1 stop bit - 1: 1.5 stop bits - 2: 2 stop bits
  uint8_t parity;    ///< 0: None - 1: Odd - 2: Even - 3: Mark - 4: Space
  uint8_t data_bits; ///< can be 5, 6, 7, 8 or 16
} uart_line_coding_t;

struct uart_driver {
  struct driver drv;

  int (*read_nbytes)(const struct device *device, uint8_t *dst, size_t len);
  int (*write_nbytes)(const struct device *device, const uint8_t *src,
                      size_t len);
  int (*read_nbytes_nonblocking)(const struct device *device, uint8_t *dst,
                                 size_t len);
  int (*write_nbytes_nonblocking)(const struct device *device,
                                  const uint8_t *src, size_t len);

  // TX/RX buffers availbility
  bool (*readable)(const struct device *device);
  bool (*writable)(const struct device *device);

  // Set baud
  void (*set_baud)(const struct device *device, unsigned int baud);
  void (*get_baud)(const struct device *device, unsigned int *baud);

  void (*set_line_coding)(const struct device *device,
                          const uart_line_coding_t *coding);

  void (*release)(const struct device *device);
  void (*acquire)(const struct device *device);
};

int uart_write_nbytes(const struct device *device, const uint8_t *src,
                      size_t len);
int uart_read_nbytes(const struct device *device, uint8_t *dst, size_t len);
int uart_write_nbytes_nonblocking(const struct device *device,
                                  const uint8_t *src, size_t len);
int uart_read_nbytes_nonblocking(const struct device *device, uint8_t *dst,
                                 size_t len);
bool uart_readable(const struct device *device);
bool uart_writable(const struct device *device);
void uart_set_baud(const struct device *device, unsigned int baud);
void uart_set_line_coding(const struct device *device,
                          const uart_line_coding_t *coding);
void uart_get_baud(const struct device *device, unsigned int *baud);
void uart_release(const struct device *device);
void uart_acquire(const struct device *device);

int uart_read_nbytes_until(const struct device *device, uint8_t *dst,
                           size_t len, uint8_t terminator);