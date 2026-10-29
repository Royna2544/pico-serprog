#include "core/devicetree.h"
#include "core/init.h"
#include "core/printk.h"
#include "drivers/wlandrv.h"
#include "pico/stdlib.h"
#include <string.h>

int main(int argc, char **argv) {
  stdio_init_all();
  __initcall_for_each(early);
  __initcall_for_each(core);
  __initcall_for_each(drivers);
  __initcall_for_each(postcore);

  struct device *wifi = device_find_by_alias("wlan0");
  if (!wifi || !wifi->platform_data) {
    pr_err("ESP device not probed");
    return 1;
  }
  if (argc == 2 && !strcmp(argv[1], "invalid")) {
    // Autoboot entered NORMAL; UPLOAD is invalid without passing through NONE.
    return wlan_set_boot_mode(wifi, WLAN_BOOT_UPLOAD) < 0 ? 0 : 2;
  }
  if (argc == 2 && !strcmp(argv[1], "reenter-normal")) {
    if (wlan_set_boot_mode(wifi, WLAN_BOOT_NONE) < 0)
      return 3;
    return wlan_set_boot_mode(wifi, WLAN_BOOT_NORMAL) < 0 ? 4 : 0;
  }
  // Autoboot may already have entered NORMAL; transitions pass through NONE.
  if (wlan_set_boot_mode(wifi, WLAN_BOOT_NONE) < 0)
    return 5;
  if (wlan_set_boot_mode(wifi, WLAN_BOOT_UPLOAD) < 0)
    return 6;
  // The integration test checks MODE esp and the wired GPIOs after this call.
  return 0;
}
