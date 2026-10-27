#pragma once

#include "printk.h"
#include <stdlib.h>

typedef int (*initcall_t)(void);
struct initcall_node {
  initcall_t func;
  const char *func_name;
};

#define _initcall(x, section)                                                  \
  static const struct initcall_node __initcall_##x                             \
      __attribute__((used, __section__("." #section))) = {.func = x,           \
                                                          .func_name = #x};

extern const struct initcall_node __start_early_init[];
extern const struct initcall_node __stop_early_init[];
extern const struct initcall_node __start_core_init[];
extern const struct initcall_node __stop_core_init[];
extern const struct initcall_node __start_drivers_init[];
extern const struct initcall_node __stop_drivers_init[];
extern const struct initcall_node __start_postcore_init[];
extern const struct initcall_node __stop_postcore_init[];

#define early_initcall(x) _initcall(x, early_init)
#define core_initcall(x) _initcall(x, core_init)
#define driver_initcall(x) _initcall(x, drivers_init)
#define postcore_initcall(x) _initcall(x, postcore_init)

static inline int run_initcalls(const struct initcall_node *first,
                                const struct initcall_node *last,
                                const char *stage) {
  pr_debug("Entering stage: %s", stage);
  for (const struct initcall_node *call = first; call != last; call++) {
    pr_debug("Calling init function: %s -> %s", stage, call->func_name);
    int rc = call->func();
    pr_debug("Init function returned: %s -> %s, rc=%d", stage, call->func_name,
             rc);
    if (rc != 0) {
      pr_debug("Exiting stage: %s", stage);
      return rc;
    }
  }
  pr_debug("Exiting stage: %s", stage);
  return 0;
}

#define __initcall_for_each(stage)                                             \
  run_initcalls(__start_##stage##_init, __stop_##stage##_init, #stage)
