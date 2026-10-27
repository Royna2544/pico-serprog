#include "printk.h"
#include <hardware/timer.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static const int loglevel = LOG_DEBUG;

int __attribute((weak)) printf_for_printk(const char *fmt, ...) {
  int n = 0;
  va_list args;
  va_start(args, fmt);
  n = vprintf(fmt, args);
  va_end(args);
  return n;
}

void __attribute__((weak)) puts_for_printk(const char *str) { puts(str); }

// Linux-style printk function for logging messages with different log levels
void printk(int level, const char *fmt, ...) {
  char buf[256]; // Buffer to hold the formatted message
  size_t n = 0;
  va_list args;

  if (level > loglevel) {
    return; // Skip messages with a log level higher than the current log level
  }

  // Generate the timestamp
  // Prefix the log level to the message
  // <4>[0.0000001] message...
  n = snprintf(buf, sizeof(buf), "<%d>[%06d.%06ld] [%d:suffering/%d: 1] ",
               level, (int)(time_us_64() / 1000000ULL),
               (long)(time_us_64() % 1000000ULL), get_core_num(),
               get_core_num());

  // Format the rest of the message
  va_start(args, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
  va_end(args);

  // Print the final message
  if (level <= LOG_ERR) {
    // ANSI Red
    printf_for_printk("\033[31m%s\033[0m\n", buf);
  } else if (level <= LOG_WARN) {
    // ANSI Yellow
    printf_for_printk("\033[33m%s\033[0m\n", buf);
  } else if (level == LOG_INFO) {
    // ANSI Green
    printf_for_printk("\033[32m%s\033[0m\n", buf);
  } else {
    puts_for_printk(buf);
  }
}