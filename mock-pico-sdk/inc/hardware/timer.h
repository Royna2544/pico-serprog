#pragma once

#include <stdint.h>
uint64_t time_us_64(void);
static inline int get_core_num() {
  return 0; // Always return core 0 for simplicity
}