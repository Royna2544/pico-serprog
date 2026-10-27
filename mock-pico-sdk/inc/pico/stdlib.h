#pragma once

#include "../hardware/uart.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static inline void sleep_ms(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

static inline void sleep_us(uint32_t us) {
  struct timespec ts;
  ts.tv_sec = us / 1000000;
  ts.tv_nsec = (us % 1000000) * 1000;
  nanosleep(&ts, NULL);
}
static inline void stdio_init_all(void) {}