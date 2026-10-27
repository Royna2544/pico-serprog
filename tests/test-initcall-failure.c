#include <stdio.h>
#include <string.h>

#include "core/printk.h"
#include "core/init.h"

static int exits;
void printk(int level, const char *fmt, ...) {
  (void)level;
  if (strncmp(fmt, "Exiting stage:", 14) == 0)
    exits++;
}

static int calls;

static int fail_early(void) {
  calls++;
  return -7;
}
early_initcall(fail_early);

static int should_not_run(void) {
  calls++;
  return 0;
}

int main(void) {
  const struct initcall_node ordered[] = {
      {.func = fail_early, .func_name = "fail_early"},
      {.func = should_not_run, .func_name = "should_not_run"},
  };
  int rc = run_initcalls(ordered, ordered + 2, "early");
  if (rc != -7 || calls != 1 || exits != 1) {
    fprintf(stderr, "ordered initcalls ignored failure: rc=%d calls=%d exits=%d\n",
            rc, calls, exits);
    return 1;
  }
  calls = 0;
  exits = 0;
  rc = __initcall_for_each(early);
  if (rc != -7) {
    fprintf(stderr, "initcall error was not propagated: %d\n", rc);
    return 1;
  }
  if (calls != 1) {
    fprintf(stderr, "initcalls continued after failure: %d calls\n", calls);
    return 1;
  }
  if (exits != 1) {
    fprintf(stderr, "initcall exit log omitted: %d\n", exits);
    return 1;
  }
  return 0;
}
