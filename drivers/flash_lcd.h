#ifndef FLASH_LCD_H
#define FLASH_LCD_H

#include "drivers/flash_observer.h"

#define FLASH_LCD_MIN_INTERVAL_US 200000u

/* Zero-initialize on the telemetry consumer core. This optional presenter
 * never reports a flashrom result: SPI samples are observations only.
 * Call with a copied flash_sample and a monotonic microsecond timestamp. */
struct flash_lcd {
  uint64_t last_attempt_us;
  uint32_t session;
  bool attempted, last_ok;
  char line0[17], line1[17];
};

/* Best effort: a missing/deferred display or write error never affects
 * flash/serprog results. No operation on USB or the SPI producer core. */
void flash_lcd_present(struct flash_lcd *state, const struct flash_sample *sample,
                       uint64_t now_us);

#endif
