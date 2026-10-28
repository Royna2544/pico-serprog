#ifndef WLANDRV_H
#define WLANDRV_H

#include "../core/driver.h"
#include <stddef.h>
#include <stdint.h>

enum wlan_driver_boot_mode {
  WLAN_BOOT_UNINIT, // Held in reset
  WLAN_BOOT_NONE,   // Held in reset
  WLAN_BOOT_NORMAL,
  WLAN_BOOT_UPLOAD,
  __WLAN_BOOT_TEST,
};

struct esp8266_driver {
  struct driver drv;

  int (*boot)(struct device *device, enum wlan_driver_boot_mode mode);
  int (*talk)(struct device *device, const char *data, size_t len,
              char *response, size_t response_len);
};

void wlan_set_boot_mode(struct device *device, enum wlan_driver_boot_mode mode);
int wlan_talk(struct device *device, const char *data, size_t len,
              char *response, size_t response_len);

#endif // WLANDRV_H