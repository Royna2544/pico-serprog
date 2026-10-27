/**
 * Copyright (C) 2021, Mate Kukri <km@mkukri.xyz>
 * Based on "pico-serprog" by Thomas Roth <code@stacksmashing.net>
 *
 * Licensed under GPLv3
 *
 * Also based on stm32-vserprog:
 *  https://github.com/dword1511/stm32-vserprog
 *
 */

#include "core/devicetree.h"
#include "core/init.h"
#include "core/printk.h"
#include "drivers/leddrv.h"
#include "drivers/passthru.h"
#include "drivers/serprog.h"
#include "drivers/wlandrv.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <stdio.h>

#ifndef MOCK_HARDWARE
#include "core/panic.h"
#endif

#include "drivers/uartdrv.h"
#include <sys/errno.h>

void core1_entry() {
  struct device *serprog_dev = device_find_by_alias("serprog0");
  if (!serprog_dev) {
    pr_err("Failed to find serprog device");
    return;
  }
  serprogdrv_process_commands(serprog_dev);
}

#ifdef MOCK_HARDWARE
#include "boardstate-daemon/client.h"
#include <signal.h>
void sigint_handler(int signum) {
  (void)signum;
  pr_info("SIGINT received, exiting...");
  signal(SIGINT, SIG_DFL);
  exit(0);
}
#endif

#include "esp8266/wlantalk.h"

static int wlantalk_getc_func(void *privdata) {
  struct device *dev = (struct device *)privdata;
  uint8_t c;
  int rc = wlan_talk(dev, NULL, 0, (char *)&c, 1);
  if (rc < 0) {
    pr_err("Failed to read byte from UART: %d", rc);
    return -1;
  }
  return (int)c;
}

static void wlantalk_putc_func(void *privdata, int c) {
  struct device *dev = (struct device *)privdata;
  uint8_t byte = (uint8_t)c;
  int rc = wlan_talk(dev, (const char *)&byte, 1, NULL, 0);
  if (rc < 0) {
    pr_err("Failed to write byte to UART: %d", rc);
  }
}

static void wlan_talk_loop(struct device *wlan_dev) {
  struct wlan_talk_message msg;
#define send(command, data)                                                    \
  make_wlan_talk_message(wlantalk_putc_func, wlan_dev, command, data)
#define is(s) (!strcmp(msg.command, s))
#define get() parse_wlan_talk_message(wlantalk_getc_func, wlan_dev, &msg)
#define send_get(command, data)                                                \
  send(command, data);                                                         \
  if (get() < 0) {                                                             \
    pr_err("Failed to parse WLAN talk message");                               \
    return;                                                                    \
  }

  send_get("SLAVE", "pico-serprog");
  pr_info("Slave device is %s", msg.data);
  send_get("ADDR", "");
  pr_info("Slave device MAC: %s", msg.data);
  send_get("SCAN", "");
  pr_info("Slave device scan results: %s", msg.data);
  send_get("CONN?", "");
  pr_info("Slave device connection status: %s", msg.data);

  while (0) {
    if (make_wlan_talk_message(wlantalk_putc_func, wlan_dev, msg.command,
                               msg.data) < 0) {
      pr_err("Failed to make WLAN talk message");
    }
  }
#undef send
#undef is
#undef get
}

int main() {
  stdio_init_all();
#ifdef MOCK_HARDWARE
  sleep_ms(5);
  if (board_reset_pico() < 0) {
    pr_err("Failed to reset virtual Pico board");
    return EXIT_FAILURE;
  }
#endif
#ifndef MOCK_HARDWARE
  printf("\x1b[2J\x1b[3J\x1b[H");
#endif

  pr_info("Start, pico-serprog-with-wifi " __DATE__ " " __TIME__);
  pr_info("Build features: +WLAN +ESP8266 +SUFFERING -SANITY +LEDRGB +SMP "
          "+SDCARD ++LINUX");

  int rc;
  if ((rc = __initcall_for_each(early)) || (rc = __initcall_for_each(core)) ||
      (rc = __initcall_for_each(drivers)) ||
      (rc = __initcall_for_each(postcore))) {
#ifdef MOCK_HARDWARE
    pr_err("Initialization failed: %d", rc);
    return EXIT_FAILURE;
#else
    do_panic("Initialization failed: %d", rc);
#endif
  }

  extern void drvcore_exit(void);
  atexit(drvcore_exit);

  multicore_launch_core1(core1_entry);
#ifdef MOCK_HARDWARE
  signal(SIGINT, sigint_handler);
#endif

  struct device *led_dev = device_find_by_alias("led0");
  if (!led_dev) {
    pr_err("Failed to find led device");
    return EXIT_FAILURE;
  }
  struct led_device_driver *led_drv =
      container_of(led_dev->drv, struct led_device_driver, drv);
  led_set_state(led_dev, true); // Turn on the LED

  struct device *wlan_dev = device_find_by_alias("wlan0");
  if (!wlan_dev) {
    pr_err("Failed to find wlan device");
    return EXIT_FAILURE;
  }

  enum wlan_driver_boot_mode bootmode = WLAN_BOOT_NORMAL;
  wlan_set_boot_mode(wlan_dev, bootmode);
  switch (bootmode) {
  case WLAN_BOOT_NORMAL: {
    wlan_talk_loop(wlan_dev);
    break;
  }
  case WLAN_BOOT_UPLOAD: {
    struct device *passthru_dev = device_find_by_alias("passthru0");
    if (!passthru_dev) {
      pr_err("Failed to find passthru device");
      return EXIT_FAILURE;
    }

    bool stop = false;
    passthru_handle_loop(passthru_dev, &stop);
    break;
  }
  default:
    pr_err("Unknown boot mode: %d", bootmode);
    return EXIT_FAILURE;
  }
}
