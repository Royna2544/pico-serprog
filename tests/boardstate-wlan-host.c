#include "core/devicetree.h"
#include "core/init.h"
#include "core/printk.h"
#include "drivers/wlandrv.h"
#include "pico/stdlib.h"

int main(void) {
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
  wlan_set_boot_mode(wifi, WLAN_BOOT_UPLOAD);
  return ((struct esp8266_platform_data *)wifi->platform_data)->cur_boot_mode ==
                 WLAN_BOOT_UPLOAD
             ? 0
             : 2;
}
