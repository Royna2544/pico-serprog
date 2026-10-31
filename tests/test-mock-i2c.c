#include "hardware/i2c.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  const uint8_t bytes[] = {0x28, 0x0c};
  assert(i2c_write_blocking((i2c_inst_t *)i2c0, 0x3f, bytes, sizeof(bytes), false) ==
         sizeof(bytes));
  puts("PASS mock I2C writes report the byte count");
  return 0;
}
