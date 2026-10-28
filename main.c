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
#include "drivers/latched-switch.h"
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

#include <pico/util/queue.h>

queue_t event_queue;
int count = 0;
static void spi_transfer_callback(const struct device *dev, const uint8_t *send,
                                  uint32_t slen, uint8_t *reply,
                                  uint32_t rlen) {
  if (slen > 0) {
    // READ JEDEC ID command (0x9F) is 1 byte, and the response is 3 bytes.
    char event[128];
    if (slen == 1 && send[0] == 0x9F && rlen == 3) {
      snprintf(event, sizeof(event), "JEDEC_ID: %02X %02X %02X %d", reply[0],
               reply[1], reply[2], count);
      pr_debug("%s", event);
      if (!queue_try_add(&event_queue, event)) {
        pr_err("Failed to add event to queue");
      }
      count++;
    } else {
      event[0] = '\0';
    }
  }
}
void serprog_command_callback(const struct device *dev, uint8_t cmd) {}

void core1_entry() {
  const char *serprog_dev_alias;
#ifdef MOCK_HARDWARE
  serprog_dev_alias = "serprogfake1";
#else
  serprog_dev_alias = "serprog0";
#endif
  struct device *serprog_dev = device_find_by_alias(serprog_dev_alias);
  if (!serprog_dev) {
    pr_err("Failed to find serprog device");
    return;
  }
  serprogdrv_set_spi_transfer_callback(serprog_dev, spi_transfer_callback);
  serprogdrv_set_command_callback(serprog_dev, serprog_command_callback);
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
#define data_is(s) (!strcmp(msg.data, s))
#define get() parse_wlan_talk_message(wlantalk_getc_func, wlan_dev, &msg)
#define send_get(command, data)                                                \
  send(command, data);                                                         \
  if (get() < 0) {                                                             \
    pr_err("WLAN TALK: " #command " failed");                                  \
    return;                                                                    \
  }

  // Slave info
  send_get("SLAVE", "pico-serprog");
  pr_info("Slave device is %s", msg.data);
  send_get("ADDR", "");
  pr_info("Slave device MAC: %s", msg.data);

  // Check connection status
  send_get("CONN?", "");
  pr_info("Slave device connection status: %s", msg.data);
  // Turn on STA mode and connect to the AP
  send_get("CONN", "DNStime,110711na!@,10000");
  if (data_is("OK")) {
    pr_info("Slave device STA mode enabled");
  } else {
    pr_err("Slave device STA mode enable failed: %s", msg.data);
    return;
  }
  send_get("CONN?", "");
  pr_info("Slave device re-check connection status: %s", msg.data);

  send_get("SCAN", "");
  pr_info("Slave device scan results: %s", msg.data);
  send_get("IPADDR", "");
  pr_info("Slave device IP address: %s", msg.data);
  send_get("RSSI", "");
  pr_info("Slave device RSSI: %s", msg.data);
  send_get("SSID", "");
  pr_info("Slave device SSID: %s", msg.data);
  send_get("SDTEST", "");
  pr_info("Slave device SD test result: %s", msg.data);
  send_get("SRVIT", "8000");
  pr_info("Slave device Server: %s", msg.data);
  if (!queue_init(&event_queue, 128, 1)) {
    pr_err("Failed to initialize event queue");
    return;
  }

  while (1) {
    char event_l[128];
    queue_remove_blocking(&event_queue, event_l);
    send("EVT", event_l);
    pr_info("Sent event to slave device: %s", event_l);
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
  led_set_state(led_dev, true); // Turn on the LED

  struct device *bootmode_switch = device_find_by_alias("switch0");
  if (!bootmode_switch) {
    pr_err("Failed to find boot mode switch device");
    return EXIT_FAILURE;
  }
  bool switch_state;
  if (latched_switch_get_state(bootmode_switch, &switch_state) != 0) {
    pr_err("Failed to get boot mode switch state");
    return EXIT_FAILURE;
  }
  enum wlan_driver_boot_mode bootmode =
      switch_state ? WLAN_BOOT_NORMAL : WLAN_BOOT_UPLOAD;
  pr_info("Wlan boot mode: %s",
          bootmode == WLAN_BOOT_NORMAL ? "NORMAL" : "UPLOAD");

  struct device *wlan_dev = device_find_by_alias("wlan0");
  if (!wlan_dev) {
    pr_err("Failed to find wlan device");
    return EXIT_FAILURE;
  }

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
