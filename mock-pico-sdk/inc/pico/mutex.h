
#include <stdint.h>

typedef struct TU_ATTR_PACKED {
  uint32_t bit_rate;
  uint8_t stop_bits; ///< 0: 1 stop bit - 1: 1.5 stop bits - 2: 2 stop bits
  uint8_t parity;    ///< 0: None - 1: Odd - 2: Even - 3: Mark - 4: Space
  uint8_t data_bits; ///< can be 5, 6, 7, 8 or 16
} cdc_line_coding_t;

#include <pthread.h>

#define CFG_TUSB_MCU 1
typedef pthread_mutex_t mutex_t;
void mutex_init(mutex_t *mutex) { pthread_mutex_init(mutex, NULL); }

void mutex_enter_blocking(mutex_t *mutex) { pthread_mutex_lock(mutex); }

void mutex_exit(mutex_t *mutex) { pthread_mutex_unlock(mutex); }

bool mutex_try_enter(mutex_t *mutex, int *owner) {
  return pthread_mutex_trylock(mutex) == 0;
}
