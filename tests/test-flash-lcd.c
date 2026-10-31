/* Host fixture: production presenter, with only LCD methods and alias lookup
 * replaced at the hardware/device-core boundary. Build commands are in the
 * execution report; TEST_REAL_LCD separately exercises the real LCD driver. */
#ifndef TEST_REAL_LCD
#include "drivers/flash_lcd.h"
#include "drivers/pcf8574t_16x2_lcd.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct device display;
static struct pcf8574t_16x2_lcd_driver methods;
static struct device *available = &display;
static char rows[2][17];
static unsigned int cursor_calls, write_calls;
static unsigned int fail_write_on_call;
static unsigned int active_row;

struct device *device_find_by_alias(const char *alias) {
  assert(strcmp(alias, "display0") == 0);
  return available;
}

static int set_cursor(const struct device *dev, uint8_t row, uint8_t col) {
  assert(dev == &display);
  assert(row < 2 && col == 0);
  cursor_calls++;
  active_row = row;
  return 0;
}

static int write_string(const struct device *dev, const char *str) {
  assert(dev == &display);
  assert(strlen(str) == 16);
  write_calls++;
  if (write_calls == fail_write_on_call)
    return -EIO;
  memcpy(rows[active_row], str, 17);
  return 0;
}

static void reset_display(void) {
  memset(rows, '.', sizeof(rows));
  rows[0][16] = rows[1][16] = 0;
  cursor_calls = write_calls = active_row = 0;
  fail_write_on_call = 0;
  available = &display;
  methods = (struct pcf8574t_16x2_lcd_driver){
      .drv.name = "pcf8574t-16x2-lcd-drv",
      .set_cursor = set_cursor, .write_string = write_string};
  display = (struct device){
      .drv = &methods.drv, .platform_data = &methods,
      .compatible = "royna,pcf8574t-16x2-lcd"};
}

static void test_id_is_observation_and_full_width(void) {
  reset_display();
  struct flash_lcd state = {0};
  struct flash_sample sample = {.kind = FL_ID, .session = 1};
  flash_lcd_present(&state, &sample, 100);
  assert(cursor_calls == 2 && write_calls == 2);
  assert(strcmp(rows[0], "ID observed     ") == 0);
  assert(strcmp(rows[1], "E:0 P:0         ") == 0);
}

static void test_erase_phase_replaces_old_text_and_limits_width(void) {
  reset_display();
  struct flash_lcd state = {0};
  struct flash_sample sample = {.kind = FL_ID, .session = 1};
  flash_lcd_present(&state, &sample, 100);
  sample.kind = FL_SE;
  sample.erase4k = 2048;
  sample.program256 = 32768;
  flash_lcd_present(&state, &sample, 200);
  assert(cursor_calls == 4 && write_calls == 4);
  assert(strcmp(rows[0], "Erase observed  ") == 0);
  assert(strcmp(rows[1], "E:2048 P:32768  ") == 0);
}

static void expect_row(unsigned int row, const char *text) {
  char expected[17];
  assert(strlen(text) <= 16);
  memset(expected, ' ', 16);
  memcpy(expected, text, strlen(text));
  expected[16] = 0;
  if (strcmp(rows[row], expected) != 0)
    fprintf(stderr, "row %u: got [%s], expected [%s]\n", row, rows[row], expected);
  assert(strcmp(rows[row], expected) == 0);
}

static void test_all_phases_describe_observed_spi_not_flashrom_result(void) {
  static const struct {
    enum flash_kind kind;
    const char *label, *count;
  } cases[] = {
      {FL_CE, "Erase observed", "E:5 P:7"},
      {FL_BE32, "Erase observed", "E:5 P:7"},
      {FL_BE64, "Erase observed", "E:5 P:7"},
      {FL_PP, "Program observed", "E:5 P:7"},
      {FL_RD, "Read observed", "R:42"},
      {FL_BUSY, "Busy observed", "E:5 P:7"},
      {FL_READY, "Ready observed", "E:5 P:7"},
      {FL_IDLE, "SPI idle", "E:5 P:7"},
      {FL_UNSUP, "ID unsupported", "E:5 P:7"},
  };
  reset_display();
  struct flash_lcd state = {0};
  struct flash_sample sample = {
      .kind = FL_ID, .session = 1, .erase4k = 5,
      .program256 = 7, .read256 = 42};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    sample.kind = cases[i].kind;
    flash_lcd_present(&state, &sample, 300000u * (i + 1));
    expect_row(0, cases[i].label);
    expect_row(1, cases[i].count);
  }
}

static void test_duplicate_and_rate_limited_updates(void) {
  reset_display();
  struct flash_lcd state = {0};
  struct flash_sample sample = {.kind = FL_ID, .session = 1};
  flash_lcd_present(&state, &sample, 0);
  flash_lcd_present(&state, &sample, 1);
  assert(write_calls == 2); /* Identical sample is not re-sent. */

  sample.kind = FL_SE;
  flash_lcd_present(&state, &sample, 2);
  assert(write_calls == 4); /* New phase can be shown promptly. */
  sample.erase4k = 1;
  flash_lcd_present(&state, &sample, 3);
  assert(write_calls == 4); /* Counter churn is throttled. */
  flash_lcd_present(&state, &sample, 2 + FLASH_LCD_MIN_INTERVAL_US);
  assert(write_calls == 6);
  expect_row(1, "E:1 P:0");
  flash_lcd_present(&state, &sample, 3000000);
  assert(write_calls == 6); /* No perpetual I2C traffic. */
}

static void test_missing_or_deferred_display_is_optional_and_recovers(void) {
  reset_display();
  struct flash_lcd state = {0};
  struct flash_sample sample = {.kind = FL_ID, .session = 1};
  available = NULL;
  flash_lcd_present(&state, &sample, 1);
  available = &display;
  display.deferred = true;
  flash_lcd_present(&state, &sample, 2);
  display.deferred = false;
  display.platform_data = NULL;
  flash_lcd_present(&state, &sample, 3);
  assert(write_calls == 0 && cursor_calls == 0);

  display.platform_data = &methods;
  flash_lcd_present(&state, &sample, 4);
  assert(write_calls == 2);
  available = NULL;
  flash_lcd_present(&state, &sample, 5);
  assert(write_calls == 2);
  available = &display;
  flash_lcd_present(&state, &sample, 6);
  assert(write_calls == 4); /* Same sample must repaint after removal. */
  display.deferred = true;
  flash_lcd_present(&state, &sample, 7);
  display.deferred = false;
  flash_lcd_present(&state, &sample, 8);
  assert(write_calls == 6);
}

static void test_write_failure_never_propagates_and_retries_slowly(void) {
  reset_display();
  struct flash_lcd state = {0};
  struct flash_sample sample = {.kind = FL_ID, .session = 1};
  fail_write_on_call = 2; /* Simulate a real LCD method returning I2C -EIO. */
  flash_lcd_present(&state, &sample, 10);
  assert(write_calls == 2 && cursor_calls == 2);
  assert(!state.last_ok && sample.kind == FL_ID);

  sample.kind = FL_SE;
  flash_lcd_present(&state, &sample, 11);
  assert(write_calls == 2); /* Failed bus also gets a retry cooldown. */
  flash_lcd_present(&state, &sample, 10 + FLASH_LCD_MIN_INTERVAL_US);
  assert(write_calls == 4 && state.last_ok);
  expect_row(0, "Erase observed");
  expect_row(1, "E:0 P:0");
}

static void test_alias_with_wrong_driver_is_ignored(void) {
  reset_display();
  struct flash_lcd state = {0};
  struct flash_sample sample = {.kind = FL_ID, .session = 1};
  struct driver unrelated = {.name = "not-an-lcd"};
  display.drv = &unrelated;
  flash_lcd_present(&state, &sample, 1);
  assert(write_calls == 0 && cursor_calls == 0);
}

int main(void) {
  test_id_is_observation_and_full_width();
  test_erase_phase_replaces_old_text_and_limits_width();
  test_all_phases_describe_observed_spi_not_flashrom_result();
  test_duplicate_and_rate_limited_updates();
  test_missing_or_deferred_display_is_optional_and_recovers();
  test_write_failure_never_propagates_and_retries_slowly();
  test_alias_with_wrong_driver_is_ignored();
  puts("PASS flash LCD presenter");
  return 0;
}
#else /* Test the production PCF8574T driver itself, not the presenter. */
#include "drivers/pcf8574t_16x2_lcd.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static unsigned int i2c_calls, short_on_call;
static uint8_t written[6];
static struct device *probe_i2c;

int i2c_device_write(const struct device *dev, const uint8_t *bytes, size_t len) {
  (void)dev;
  assert(len == 1);
  ++i2c_calls;
  if (i2c_calls <= sizeof(written))
    written[i2c_calls - 1] = bytes[0];
  return i2c_calls == short_on_call ? 0 : 1;
}

struct device *find_device_by_prop(struct device *dev, const char *property,
                                   int offset, int index) {
  (void)dev;
  (void)property;
  (void)offset;
  (void)index;
  return probe_i2c;
}

bool driver_register(struct driver *drv) {
  (void)drv;
  return true;
}

void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}

static void test_short_i2c_write_is_an_error(void) {
  struct device i2c_dev = {0};
  struct pcf8574t_16x2_lcd_pdata pdata = {.i2c_dev = &i2c_dev};
  struct device display_dev = {.platform_data = &pdata};
  i2c_calls = 0;
  short_on_call = 5;
  int rc = lcd_write_char(&display_dev, 'A');
  assert(rc < 0 && i2c_calls == 5);
}

static void test_backpack_control_bits_are_initialized(void) {
  struct device i2c_dev = {0};
  struct pcf8574t_16x2_lcd_pdata pdata = {.i2c_dev = &i2c_dev};
  struct device display_dev = {.platform_data = &pdata};
  i2c_calls = short_on_call = 0;
  assert(lcd_write_char(&display_dev, 'A') == 0 && i2c_calls == 6);
  for (size_t i = 0; i < sizeof(written); ++i)
    assert((written[i] & 0x0b) == 0x09); /* RS=1, RW=0, backlight P3=1. */

  i2c_calls = 0;
  assert(lcd_clear(&display_dev) == 0 && i2c_calls == 6);
  for (size_t i = 0; i < sizeof(written); ++i)
    assert((written[i] & 0x0b) == 0x08); /* Commands also keep P3 high. */
}

static void test_absent_i2c_defers_optional_lcd_probe(void) {
  struct device display_dev = {.fdt_node_offset = 2};
  probe_i2c = NULL;
  i2c_calls = 0;
  assert(lcd_probe(&display_dev) == EPROBE_DEFER);
  assert(display_dev.platform_data == NULL && i2c_calls == 0);
}

static void test_deferred_i2c_does_not_appear_ready(void) {
  struct device i2c_dev = {.deferred = true};
  struct device display_dev = {.fdt_node_offset = 2};
  probe_i2c = &i2c_dev;
  i2c_calls = 0;
  assert(lcd_probe(&display_dev) == EPROBE_DEFER);
  assert(display_dev.platform_data == NULL && i2c_calls == 0);
}

static void test_lcd_i2c_nack_defers_without_stopping_firmware(void) {
  struct device i2c_dev = {0};
  struct device display_dev = {.fdt_node_offset = 2};
  i2c_dev.platform_data = &i2c_dev;
  probe_i2c = &i2c_dev;
  i2c_calls = 0;
  short_on_call = 1;
  assert(lcd_probe(&display_dev) == EPROBE_DEFER);
  assert(i2c_calls == 1 && display_dev.platform_data == NULL);
  assert(written[0] & 0x08); /* First initialization write lights the display. */
  short_on_call = 0;
}

int main(void) {
  test_short_i2c_write_is_an_error();
  test_backpack_control_bits_are_initialized();
  test_absent_i2c_defers_optional_lcd_probe();
  test_deferred_i2c_does_not_appear_ready();
  test_lcd_i2c_nack_defers_without_stopping_firmware();
  puts("PASS real PCF8574T LCD driver");
  return 0;
}
#endif
