#include "drivers/flash_lcd.h"
#include "drivers/pcf8574t_16x2_lcd.h"

#include <stdio.h>
#include <string.h>

static void pad_line(char out[17], const char *text) {
  size_t n = strlen(text);
  if (n > 16)
    n = 16;
  memset(out, ' ', 16);
  memcpy(out, text, n);
  out[16] = '\0';
}

void flash_lcd_present(struct flash_lcd *state, const struct flash_sample *sample,
                       uint64_t now_us) {
  (void)now_us;
  if (!state || !sample)
    return;

  struct device *dev = device_find_by_alias("display0");
  if (!dev || dev->deferred || !dev->drv || !dev->platform_data ||
      !dev->drv->name ||
      strcmp(dev->drv->name, "pcf8574t-16x2-lcd-drv") != 0) {
    /* A later probe/reconnect must repaint even if the sample is unchanged. */
    state->attempted = false;
    state->last_ok = false;
    return;
  }
  const struct pcf8574t_16x2_lcd_driver *lcd =
      container_of(dev->drv, struct pcf8574t_16x2_lcd_driver, drv);
  if (!lcd->set_cursor || !lcd->write_string) {
    state->attempted = false;
    state->last_ok = false;
    return;
  }

  char line0[17], line1[17], counts[32];
  const char *phase;
  switch (sample->kind) {
  case FL_ID: phase = "ID observed"; break;
  case FL_UNSUP: phase = "ID unsupported"; break;
  case FL_CE:
  case FL_SE:
  case FL_BE32:
  case FL_BE64: phase = "Erase observed"; break;
  case FL_PP: phase = "Program observed"; break;
  case FL_RD: phase = "Read observed"; break;
  case FL_BUSY: phase = "Busy observed"; break;
  case FL_READY: phase = "Ready observed"; break;
  case FL_IDLE: phase = "SPI idle"; break;
  default: phase = "SPI observed"; break;
  }
  pad_line(line0, phase);
  if (sample->kind == FL_RD)
    snprintf(counts, sizeof(counts), "R:%u", sample->read256);
  else
    snprintf(counts, sizeof(counts), "E:%u P:%u", sample->erase4k,
             sample->program256);
  pad_line(line1, counts);

  if (state->attempted) {
    bool same = state->last_ok && state->session == sample->session &&
                strcmp(state->line0, line0) == 0 &&
                strcmp(state->line1, line1) == 0;
    if (same)
      return;
    /* A new phase/session is useful immediately after a successful update;
     * counter-only changes and failed writes are rate-limited. */
    bool phase_changed = state->last_ok &&
                         (state->session != sample->session ||
                          strcmp(state->line0, line0) != 0);
    if (!phase_changed && now_us >= state->last_attempt_us &&
        now_us - state->last_attempt_us < FLASH_LCD_MIN_INTERVAL_US)
      return;
  }
  state->last_attempt_us = now_us;
  state->attempted = true;
  state->last_ok = false;
  if (lcd->set_cursor(dev, 0, 0) < 0 || lcd->write_string(dev, line0) < 0 ||
      lcd->set_cursor(dev, 1, 0) < 0 || lcd->write_string(dev, line1) < 0)
    return;
  state->last_ok = true;
  state->session = sample->session;
  memcpy(state->line0, line0, sizeof(line0));
  memcpy(state->line1, line1, sizeof(line1));
}
