
#include "core/driver.h"
#include "inc/hardware/uart.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

uint64_t time_us_64(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

#include "boardstate-daemon/client.h"
#include "hardware/uart.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct uart_state {
  const char *path;
  int fd;
  unsigned int baudrate;
};

static struct uart_state uart0_state = {
    .path = "/tmp/mock-uart0",
    .fd = -1,
};

static struct uart_state uart1_state = {
    .path = "/tmp/mock-uart1",
    .fd = -1,
};

static struct uart_state *get_state(void *hw) {
  if (hw == uart0)
    return &uart0_state;

  if (hw == uart1)
    return &uart1_state;

  errno = ENODEV;
  return NULL;
}

static int get_fd(void *hw) {
  struct uart_state *s = get_state(hw);

  if (!s)
    return -1;

  if (s->fd < 0) {
    errno = ENODEV;
    return -1;
  }

  return s->fd;
}

static int wait_ready(int fd, short events) {
  struct pollfd pfd = {
      .fd = fd,
      .events = events,
  };

  for (;;) {
    int ret = poll(&pfd, 1, -1);

    if (ret < 0) {
      if (errno == EINTR)
        continue;

      return -1;
    }

    if (pfd.revents & POLLNVAL) {
      errno = EBADF;
      return -1;
    }

    if (pfd.revents & events)
      return 0;

    if (pfd.revents & (POLLERR | POLLHUP)) {
      errno = EIO;
      return -1;
    }
  }
}

static bool is_ready(void *hw, short events) {
  int fd = get_fd(hw);

  if (fd < 0)
    return false;

  struct pollfd pfd = {
      .fd = fd,
      .events = events,
  };

  int ret;

  do {
    ret = poll(&pfd, 1, 0);
  } while (ret < 0 && errno == EINTR);

  return ret > 0 && (pfd.revents & events);
}

bool uart_is_enabled(void *hw) {
  struct uart_state *s = get_state(hw);
  return s && s->fd != -1;
}

unsigned int uart_init(void *hw, unsigned int baud) {
  struct uart_state *s = get_state(hw);

  if (!s)
    return 0;
  if (hw == uart1) {
    if (board_power() < 0)
      return 0;
    s->fd = -2;
    s->baudrate = baud;
    return baud;
  }
  if (s->fd < 0) {
    const char *dir = getenv("MOCK_PTY_DIR");
    char path[PATH_MAX];
    const char *name = s->path;
    if (dir && *dir) {
      int len = snprintf(path, sizeof(path), "%s/mock-uart0", dir);
      if (len < 0 || (size_t)len >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return 0;
      }
      name = path;
    }
    int fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd < 0)
      return 0;

    struct termios tty;

    if (tcgetattr(fd, &tty) < 0) {
      int saved = errno;
      close(fd);
      errno = saved;
      return 0;
    }

    cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL | CREAD;

    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
      int saved = errno;
      close(fd);
      errno = saved;
      return 0;
    }

    s->fd = fd;
  }

  s->baudrate = baud;
  return baud;
}

int uart_getc(void *hw) {
  uint8_t c;
  int ret = uart_read_blocking(hw, &c, 1);
  if (ret < 0)
    return -1;
  if (ret == 0) {
    errno = EAGAIN;
    return -1;
  }
  return c;
}

int uart_putc(void *hw, char c) {
  int ret = uart_write_blocking(hw, (const uint8_t *)&c, 1);
  if (ret < 0)
    return -1;
  if (ret == 0) {
    errno = EAGAIN;
    return -1;
  }
  return c;
}

unsigned int uart_set_baudrate(void *hw, unsigned int baud) {
  struct uart_state *s = get_state(hw);

  if (!s)
    return 0;

  if (s->fd == -1) {
    errno = ENODEV;
    return 0;
  }

  s->baudrate = baud;
  return baud;
}

void uart_deinit(void *hw) {
  struct uart_state *s = get_state(hw);

  if (!s)
    return;

  if (s->fd >= 0)
    close(s->fd);

  s->fd = -1;
  s->baudrate = 0;
}

static int uart1_peek = -1;
bool uart_is_readable(void *hw) {
  if (hw == uart1) {
    if (board_power() != 1) {
      uart1_peek = -1;
      return false;
    }
    if (uart1_peek >= 0)
      return true;
    errno = 0;
    uart1_peek = board_uart("pico", "RX", -1);
    return uart1_peek >= 0;
  }
  return is_ready(hw, POLLIN);
}
bool uart_is_writable(void *hw) {
  return hw == uart1 ? uart_is_enabled(hw) && board_power() >= 0
                     : is_ready(hw, POLLOUT);
}

int uart_read_blocking(void *hw, uint8_t *dst, size_t len) {
  if (hw == uart1) {
    for (size_t i = 0; i < len; i++) {
      int byte;
      do {
        if (board_power() != 1) {
          errno = ENODEV;
          return -1;
        }
        byte = uart1_peek;
        uart1_peek = -1;
        if (byte < 0) {
          errno = 0;
          byte = board_uart("pico", "RX", -1);
          if (byte < 0 && errno)
            return -1;
        }
        if (byte < 0)
          usleep(1000);
      } while (byte < 0);
      dst[i] = byte;
    }
    return (int)len;
  }
  int fd = get_fd(hw);

  if (fd < 0)
    return -1;

  if (len > INT_MAX) {
    errno = EINVAL;
    return -1;
  }

  size_t total = 0;

  while (total < len) {
    ssize_t n = read(fd, dst + total, len - total);

    if (n > 0) {
      total += (size_t)n;
      continue;
    }

    if (n == 0) {
      errno = EIO;
      return -1;
    }

    if (errno == EINTR)
      continue;

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (wait_ready(fd, POLLIN) < 0)
        return -1;

      continue;
    }

    return -1;
  }

  return (int)total;
}

int uart_write_blocking(void *hw, const uint8_t *src, size_t len) {
  if (hw == uart1) {
    for (size_t i = 0; i < len; i++)
      if (board_uart("pico", "TX", src[i]) < 0)
        return -1;
    return (int)len;
  }
  int fd = get_fd(hw);

  if (fd < 0)
    return -1;

  if (len > INT_MAX) {
    errno = EINVAL;
    return -1;
  }

  size_t total = 0;

  while (total < len) {
    ssize_t n = write(fd, src + total, len - total);

    if (n > 0) {
      total += (size_t)n;
      continue;
    }

    if (n < 0 && errno == EINTR)
      continue;

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (wait_ready(fd, POLLOUT) < 0)
        return -1;

      continue;
    }

    if (n == 0)
      errno = EIO;

    return -1;
  }

  return (int)total;
}
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define MOCK_CDC_COUNT 3
#define MOCK_CDC_TX_SIZE 64

static int cdc_fds[MOCK_CDC_COUNT] = {-1, -1, -1};

static int cdc_get_fd(uint8_t itf) {
  if (itf >= MOCK_CDC_COUNT)
    return -1;

  if (cdc_fds[itf] >= 0)
    return cdc_fds[itf];

  char path[PATH_MAX];
  const char *dir = getenv("MOCK_PTY_DIR");
  if (!dir || !*dir)
    dir = "/tmp";
  int len = snprintf(path, sizeof(path), "%s/mock-cdc%u", dir, itf);
  if (len < 0 || (size_t)len >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
    return -1;

  struct termios tty;
  if (tcgetattr(fd, &tty) < 0) {
    close(fd);
    return -1;
  }

  cfmakeraw(&tty);
  tty.c_cflag |= CLOCAL | CREAD;

  if (tcsetattr(fd, TCSANOW, &tty) < 0) {
    close(fd);
    return -1;
  }

  cdc_fds[itf] = fd;
  return fd;
}

static bool cdc_poll(uint8_t itf, short events) {
  int fd = cdc_get_fd(itf);
  if (fd < 0)
    return false;

  struct pollfd pfd = {.fd = fd, .events = events};

  int ret;
  do {
    ret = poll(&pfd, 1, 0);
  } while (ret < 0 && errno == EINTR);

  return ret > 0 && (pfd.revents & events);
}

void tusb_init(void) {
  for (uint8_t i = 0; i < MOCK_CDC_COUNT; i++) {
    if (cdc_get_fd(i) < 0)
      fprintf(stderr, "mock USB: CDC%u unavailable\n", i);
  }
}
uint32_t tud_cdc_n_available(uint8_t itf) {
  int fd = cdc_get_fd(itf);
  if (fd < 0)
    return 0;

  int count = 0;
  if (ioctl(fd, FIONREAD, &count) < 0)
    return 0;

  return count > 0 ? (uint32_t)count : 0;
}

uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t size) {
  int fd = cdc_get_fd(itf);
  if (fd < 0 || !buffer || !size)
    return 0;

  ssize_t n;
  do {
    n = read(fd, buffer, size);
  } while (n < 0 && errno == EINTR);

  return n > 0 ? (uint32_t)n : 0;
}

uint32_t tud_cdc_n_read_flush(uint8_t itf) {
  uint8_t buf[256];
  uint32_t total = 0;

  while (cdc_poll(itf, POLLIN)) {
    uint32_t n = tud_cdc_n_read(itf, buf, sizeof(buf));

    if (!n)
      break;

    total += n;
  }

  return total;
}

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define CDC_TX_CAPACITY 1024

static struct {
  uint8_t data[CDC_TX_CAPACITY];
  size_t head;
  size_t count;
} cdc_tx[MOCK_CDC_COUNT];

uint32_t tud_cdc_n_write_available(uint8_t itf) {
  if (itf >= MOCK_CDC_COUNT || cdc_get_fd(itf) < 0)
    return 0;
  return CDC_TX_CAPACITY - cdc_tx[itf].count;
}

uint32_t tud_cdc_n_write(uint8_t itf, const void *buffer, uint32_t size) {
  if (itf >= MOCK_CDC_COUNT || !buffer || cdc_get_fd(itf) < 0)
    return 0;

  typeof(cdc_tx[0]) *tx = &cdc_tx[itf];

  size_t n = size;
  size_t free_space = CDC_TX_CAPACITY - tx->count;

  if (n > free_space)
    n = free_space;

  size_t tail = (tx->head + tx->count) % CDC_TX_CAPACITY;
  size_t first = CDC_TX_CAPACITY - tail;

  if (first > n)
    first = n;

  memcpy(tx->data + tail, buffer, first);
  memcpy(tx->data, (const uint8_t *)buffer + first, n - first);

  tx->count += n;
  return (uint32_t)n;
}

uint32_t tud_cdc_n_write_flush(uint8_t itf) {
  if (itf >= MOCK_CDC_COUNT)
    return 0;

  int fd = cdc_get_fd(itf);
  if (fd < 0)
    return 0;

  typeof(cdc_tx[0]) *tx = &cdc_tx[itf];
  uint32_t sent = 0;

  while (tx->count) {
    size_t chunk = CDC_TX_CAPACITY - tx->head;

    if (chunk > tx->count)
      chunk = tx->count;

    ssize_t n = write(fd, tx->data + tx->head, chunk);

    if (n > 0) {
      tx->head = (tx->head + n) % CDC_TX_CAPACITY;
      tx->count -= n;
      sent += n;
      continue;
    }

    if (n < 0 && errno == EINTR)
      continue;

    /* Retain unsent bytes for the next attempt. */
    break;
  }

  return sent;
}

void tud_task(void) {
  for (uint8_t i = 0; i < MOCK_CDC_COUNT; ++i)
    tud_cdc_n_write_flush(i);
}

void tud_task_ext(uint32_t timeout_ms, bool in_isr) {
  (void)in_isr;

  struct pollfd fds[MOCK_CDC_COUNT];
  nfds_t count = 0;

  for (uint8_t i = 0; i < MOCK_CDC_COUNT; ++i) {
    int fd = cdc_get_fd(i);
    if (fd < 0)
      continue;

    fds[count++] = (struct pollfd){
        .fd = fd, .events = POLLIN | (cdc_tx[i].count ? POLLOUT : 0)};
  }

  int timeout = timeout_ms == UINT32_MAX
                    ? -1
                    : (timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms);

  int ret;
  do {
    ret = poll(fds, count, timeout);
  } while (ret < 0 && errno == EINTR);

  if (ret >= 0)
    tud_task();
}

#include "core/list.h"
#include "pico/stdio/driver.h"

static DEFINE_LIST_HEAD(stdio_driver_list);
#include <pthread.h>

static pthread_mutex_t stdio_driver_list_mutex = PTHREAD_MUTEX_INITIALIZER;

struct stdio_thread_data {
  stdio_driver_t *driver;
  bool stop;
};

#include <stdarg.h>
void printf_for_printk(const char *fmt, ...) {
  va_list args;
  char buf[256];
  va_start(args, fmt);

  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  printf("%s", buf);
  pthread_mutex_lock(&stdio_driver_list_mutex);
  struct list_head *list;
  list_for_each(list, &stdio_driver_list) {
    stdio_driver_t *driver = container_of(list, stdio_driver_t, list);
    if (!driver->out_chars)
      continue;
    driver->out_chars(driver, buf, strlen(buf));
    driver->out_chars(driver, "\r", 1);
  }
  pthread_mutex_unlock(&stdio_driver_list_mutex);
}

void puts_for_printk(const char *str) {
  struct list_head *list;
  pthread_mutex_lock(&stdio_driver_list_mutex);
  puts(str);

  list_for_each(list, &stdio_driver_list) {
    stdio_driver_t *driver = container_of(list, stdio_driver_t, list);
    if (!driver->out_chars)
      continue;
    driver->out_chars(driver, str, strlen(str));
    driver->out_chars(driver, "\r", 1);
  }
  pthread_mutex_unlock(&stdio_driver_list_mutex);
}
void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled) {
  pthread_mutex_lock(&stdio_driver_list_mutex);
  if (enabled) {
    list_add(&driver->list, &stdio_driver_list);

  } else {
    list_del(&driver->list);
  }
  pthread_mutex_unlock(&stdio_driver_list_mutex);
}