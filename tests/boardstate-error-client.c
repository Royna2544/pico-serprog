#include "boardstate-daemon/client.h"
#include <stddef.h>

int main(void) {
  if (board_gpio("pico", 30, "DIR", "1") >= 0)
    return 1;
  if (board_gpio("esp", 17, "GET", NULL) >= 0)
    return 1;
  return 0;
}
