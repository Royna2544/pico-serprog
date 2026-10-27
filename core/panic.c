#include "panic.h"
#include "devicetree.h"
#include "printk.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

void __attribute__((noreturn, format(printf, 1, 2))) do_panic(const char *fmt,
                                                              ...) {
  va_list args;
  char buf[256];
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  pr_emerg("Kernel panic - not syncing: Fatal exception");
  pr_emerg("PC is at %p", __builtin_return_address(0));
  pr_emerg("SP is at %p", __builtin_frame_address(0));
  pr_emerg("Message: %s", buf);
  pr_emerg("Hardware name: %s (DT)",
           hardware_model ? hardware_model : "Unknown");

  pr_emerg("Exception stack (0x%llx to 0x%llx)",
           (uintptr_t)__builtin_frame_address(0),
           (uintptr_t)__builtin_frame_address(0) + 256);
  uint8_t *sp = __builtin_frame_address(0);

  for (size_t i = 0; i < 256; i += 16) {
    char line[16 * 3 + 1];
    size_t pos = 0;

    for (size_t j = 0; j < 16; ++j)
      pos += snprintf(line + pos, sizeof(line) - pos, "%02x%s", sp[i + j],
                      j == 15 ? "" : " ");

    pr_emerg("%p: %s", sp + i, line);
  }

#ifdef MOCK_HARDWARE
  __builtin_trap();
#endif
  while (1) {
    // Infinite loop to halt the system
  }
}