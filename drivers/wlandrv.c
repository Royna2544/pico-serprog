
#include "wlandrv.h"
#include "core/devicetree.h"
#include "core/init.h"
#include "core/irq.h"
#include "drivers/latched-switch.h"
#include <drivers/uartdrv.h>
#include <hardware/gpio.h>
#include <hardware/sync.h>
#include <pico/time.h>
#include <string.h>
#include <sys/errno.h>

#include <drivers/gpiolib.h>

struct esp8266_platform_data {
  struct gpio_desc *en_pin;
  struct gpio_desc *rst_pin;
  struct gpio_desc *gpio0_pin;
  struct gpio_desc *gpio2_pin;
  struct gpio_desc *gpio15_pin;
  int bootpin_irq;
  bool autoboot_enabled;

  enum wlan_driver_boot_mode cur_boot_mode;
};

static volatile bool esp8266_ready = false;

static irqreturn_t callback(int virq, irq_event_t events, void *data) {
  if (virq == *(int *)data) {
    if ((events & IRQ_EVENT_RISING_EDGE)) {
      esp8266_ready = true;
    }
    pr_debug(
        "ESP8266: IRQ callback triggered for virq %d, events 0x%x, ready: %d",
        virq, events, esp8266_ready);
    return IRQ_HANDLED;
  }
  return IRQ_NONE;
}

static int esp8266_boot(struct device *dev, enum wlan_driver_boot_mode mode);

static void esp8266_bootsource_change_callback(void *data, bool new_state) {
  struct device *dev = (struct device *)data;
  struct esp8266_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return;
  }

  enum wlan_driver_boot_mode new_mode =
      new_state ? WLAN_BOOT_NORMAL : WLAN_BOOT_UPLOAD;

  if (new_mode != pdata->cur_boot_mode) {
    dev_info(dev, "Boot source changed, reinitializing ESP8266 to mode: %d",
             new_mode);
    int rc = esp8266_boot(dev, WLAN_BOOT_NONE);
    if (rc) {
      dev_err(dev, "Failed to reinitialize ESP8266 to NONE mode");
      return;
    }
    rc = esp8266_boot(dev, new_mode);
    if (rc) {
      dev_err(dev, "Failed to reinitialize ESP8266 to mode: %d", new_mode);
    }
  }
}

static int esp8266_probe(struct device *dev) {
  struct esp8266_platform_data *pdata = calloc(1, sizeof(*pdata));
  int rc;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -ENOMEM;
  }

  if (pdata->cur_boot_mode != WLAN_BOOT_UNINIT) {
    dev_info(dev, "ESP8266 already initialized in mode: %d",
             pdata->cur_boot_mode);
    return 0;
  }
  int irq =
      gpio_request_irq(dev, dev, dev->fdt_node_offset, "interrupts-extended");
  if (irq < 0) {
    dev_err(dev, "Failed to request IRQ for ESP8266: %d", irq);
    rc = irq;
    goto free_pdata;
  }

  pdata->bootpin_irq = irq;
  dev_info(dev, "ESP8266 GPIO IRQ requested successfully. IRQ: %d", irq);
  pdata->en_pin = gpio_request(dev, dev->fdt_node_offset, "enable-gpios");
  pdata->rst_pin = gpio_request(dev, dev->fdt_node_offset, "reset-gpios");
  pdata->gpio0_pin = gpio_request(dev, dev->fdt_node_offset, "boot-gpios");
  pdata->gpio2_pin =
      gpio_request(dev, dev->fdt_node_offset, "strap-gpio2-gpios");
  pdata->gpio15_pin =
      gpio_request(dev, dev->fdt_node_offset, "strap-gpio15-gpios");

  if (!pdata->en_pin || !pdata->rst_pin || !pdata->gpio0_pin ||
      !pdata->gpio2_pin || !pdata->gpio15_pin) {
    dev_err(dev, "Failed to request GPIOs for ESP8266");
    rc = -ENODEV;
    goto release_gpios;
  }

  rc = get_prop_bool_default(dev, "autoboot", &pdata->autoboot_enabled, false);
  if (rc) {
    goto release_gpios;
  }
  dev_info(dev, "ESP8266 autoboot is %s",
           pdata->autoboot_enabled ? "enabled" : "disabled");
  dev->platform_data = pdata;

  if (esp8266_boot(dev, WLAN_BOOT_NONE)) {
    dev_err(dev, "Failed to initialize ESP8266 in NONE mode");
    rc = -EINVAL;
    goto release_gpios;
  }

  if (pdata->autoboot_enabled) {
    struct device *bootmode_source =
        find_device_by_prop(dev, "bootmode-source", dev->fdt_node_offset, 0);
    if (!bootmode_source) {
      dev_err(dev, "Failed to find bootmode-source device");
      rc = EPROBE_DEFER;
      goto release_gpios;
    }
    bool switch_state;
    rc = latched_switch_get_state(bootmode_source, &switch_state);
    if (rc != 0) {
      dev_err(dev, "Failed to get latched switch state");
      goto release_gpios;
    }
    enum wlan_driver_boot_mode bootmode =
        switch_state ? WLAN_BOOT_NORMAL : WLAN_BOOT_UPLOAD;
    if (esp8266_boot(dev, bootmode)) {
      dev_err(dev, "Failed to initialize ESP8266 in mode: %d",
              pdata->cur_boot_mode);
      rc = -EINVAL;
      goto release_gpios;
    }
    rc = latched_switch_set_on_change_callback(
        bootmode_source, esp8266_bootsource_change_callback, dev);
    if (rc != 0) {
      dev_err(dev, "Failed to set latched switch on-change callback");
      goto release_gpios;
    }
  }

  dev_info(dev, "ESP8266 probed successfully.");
  return 0;
release_gpios:
  devm_free_irq(dev, pdata->bootpin_irq);
  gpio_desc_release(dev, pdata->en_pin);
  gpio_desc_release(dev, pdata->rst_pin);
  gpio_desc_release(dev, pdata->gpio0_pin);
  gpio_desc_release(dev, pdata->gpio2_pin);
  gpio_desc_release(dev, pdata->gpio15_pin);
free_pdata:
  free(pdata);
  return rc;
}

static int esp8266_boot(struct device *dev, enum wlan_driver_boot_mode mode) {
  struct esp8266_platform_data *pdata = dev->platform_data;

  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }

  if (pdata->cur_boot_mode == mode) {
    dev_info(dev, "ESP8266 already in the requested boot mode: %d", mode);
    return 0;
  }

  const char *mode_str = (mode == WLAN_BOOT_NONE)     ? "NONE"
                         : (mode == WLAN_BOOT_NORMAL) ? "NORMAL"
                         : (mode == WLAN_BOOT_UPLOAD) ? "UPLOAD"
                                                      : "UNKNOWN";

  dev_info(dev, "Initializing ESP8266 in mode: %s", mode_str);

  switch (mode) {
  case WLAN_BOOT_NONE:
    if (pdata->cur_boot_mode != WLAN_BOOT_NORMAL &&
        pdata->cur_boot_mode != WLAN_BOOT_UPLOAD &&
        pdata->cur_boot_mode != WLAN_BOOT_UNINIT) {
      dev_err(dev, "Invalid transition to NONE mode from current mode: %d",
              pdata->cur_boot_mode);
      return -EINVAL;
    }
    // Set initial states
    gpio_set_value(pdata->gpio0_pin, 0);  // Active-low boot pin: GPIO0 high
    gpio_set_value(pdata->gpio2_pin, 1);  // Set GPIO2 high
    gpio_set_value(pdata->gpio15_pin, 1); // Set GPIO15 enabled (low)
    gpio_set_value(pdata->rst_pin, 1);    // Hold ESP8266 in reset
    gpio_set_value(pdata->en_pin, 0);     // Disable ESP8266
    break;
  case WLAN_BOOT_NORMAL:
    if (pdata->cur_boot_mode != WLAN_BOOT_NONE) {
      dev_err(dev, "Invalid transition to NORMAL mode from current mode: %d",
              pdata->cur_boot_mode);
      return -EINVAL;
    }

    // Wait for GPIO0 to read LOW, indicating the ESP8266 is ready
    esp8266_ready = false;
    int rc = devm_request_irq(dev, pdata->bootpin_irq, callback,
                              &pdata->bootpin_irq);
    if (rc < 0) {
      dev_err(dev, "Failed to request IRQ for ESP8266: %d", rc);
      return rc;
    }
    gpio_set_value(pdata->gpio0_pin, 0);  // Active-low boot pin: GPIO0 high
    gpio_set_value(pdata->gpio2_pin, 1);  // Set GPIO2 high
    gpio_set_value(pdata->gpio15_pin, 1); // Set GPIO15 enabled (low)
    gpio_set_value(pdata->rst_pin, 0);    // Release ESP8266 from reset
    gpio_set_value(pdata->en_pin, 1);     // Enable ESP8266

    dev_info(dev, "ESP8266 waiting firmare ack..");
    gpio_reset(pdata->gpio15_pin); // Reset GPIO15 to default state (Hi-Z input)

    do {
      __wfi(); // Wait for interrupt
    } while (!esp8266_ready);

    // Release GPIO2 and GPIO15 to their default states, going back to Hi-Z
    // input mode
    gpio_reset(pdata->gpio2_pin);
    gpio_reset(pdata->gpio15_pin);
    dev_info(dev, "ESP8266 firmware ack received.");

    sleep_ms(100); // Allow some time for the ESP8266 to stabilize
    // Drain any existing data in the UART buffer before sending new data
    if (uart_readable(dev->parent) > 0) {
      char drain_buffer[64];
      int drained = uart_read_nbytes_nonblocking(
          dev->parent, (uint8_t *)drain_buffer, sizeof(drain_buffer));
      if (drained < 0) {
        dev_err(dev, "Failed to drain UART buffer: %d", drained);
        return drained;
      }
      dev_dbg(dev, "Drained %d bytes from UART buffer", drained);
    }

    break;
  case WLAN_BOOT_UPLOAD:
    if (pdata->cur_boot_mode != WLAN_BOOT_NONE) {
      dev_err(dev, "Invalid transition to UPLOAD mode from current mode: %d",
              pdata->cur_boot_mode);
      return -EINVAL;
    }
    gpio_set_value(pdata->gpio0_pin, 1);  // Active-low boot pin: GPIO0 low
    gpio_reset(pdata->gpio2_pin);         // Set GPIO2 input (Hi-Z)
    gpio_set_value(pdata->gpio15_pin, 1); // Set GPIO15 enabled (low)

    gpio_set_value(pdata->rst_pin, 0); // Release ESP8266 from reset
    gpio_set_value(pdata->en_pin, 1);  // Enable ESP8266
    break;

  case __WLAN_BOOT_TEST:
    dev_info(dev, "Testing GPIOs. order: EN, RST, GPIO0, GPIO2, GPIO15");
    gpio_set_value(pdata->gpio15_pin, 1);
    sleep_ms(500);
    gpio_set_value(pdata->gpio15_pin, 0);
    dev_info(dev, "Starting test");
    for (int i = 0; i < 5; i++) {
      struct gpio_desc *pin = NULL;
      const char *pin_name = NULL;
      switch (i) {
      case 0:
        pin = pdata->en_pin;
        pin_name = "EN";
        break;
      case 1:
        pin = pdata->rst_pin;
        pin_name = "RST";
        break;
      case 2:
        pin = pdata->gpio0_pin;
        pin_name = "GPIO0";
        break;
      case 3:
        pin = pdata->gpio2_pin;
        pin_name = "GPIO2";
        break;
      case 4:
        pin = pdata->gpio15_pin;
        pin_name = "GPIO15";
        break;
      }
      if (!pin) {
        dev_err(dev, "Invalid GPIO index: %d", i);
        return -EINVAL;
      }
      dev_info(dev, "Toggling %s: HI-LOW-HIZ", pin_name);
      gpio_set_value(pin, 1);
      sleep_ms(500);
      gpio_set_value(pin, 0);
      sleep_ms(500);
      gpio_reset(pin); // Reset to default state (Hi-Z input)
      sleep_ms(500);
      dev_info(dev, "%s toggled successfully", pin_name);

      for (int j = 0; j < 5; j++) {

        gpio_set_value(pin, 0);
      }
      dev_info(dev, "GPIO test completed successfully");
    }
  default:
    return -EINVAL;
  }
  pdata->cur_boot_mode = mode;
  return 0;
}

static int esp8266_talk(struct device *dev, const char *data, size_t len,
                        char *response, size_t response_len) {
  struct esp8266_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }

  if (pdata->cur_boot_mode != WLAN_BOOT_NORMAL) {
    dev_err(dev, "ESP8266 is not in NORMAL mode. Current mode: %d",
            pdata->cur_boot_mode);
    return -EINVAL;
  }

  if (data) {
    // Write data to UART
    int written = uart_write_nbytes(dev->parent, (const uint8_t *)data, len);
    if (written < 0) {
      dev_err(dev, "Failed to write to UART for ESP8266 talk: %d", written);
      return written;
    }
  }

  if (response) {
    // Read response from UART
    int read = uart_read_nbytes(dev->parent, (uint8_t *)response, response_len);
    if (read < 0) {
      dev_err(dev, "Failed to read from UART for ESP8266 talk: %d", read);
      return read;
    }
  }

  return 0; // Return the number of bytes read
}

static void esp8266_remove(struct device *dev) {
  struct esp8266_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return;
  }

  gpio_desc_release(dev, pdata->en_pin);
  gpio_desc_release(dev, pdata->rst_pin);
  gpio_desc_release(dev, pdata->gpio0_pin);
  gpio_desc_release(dev, pdata->gpio2_pin);
  gpio_desc_release(dev, pdata->gpio15_pin);

  devm_free_irq(dev, pdata->bootpin_irq);

  free(pdata);
  dev->platform_data = NULL;
}
const static struct driver_fdt_match_id esp8266_match_table[] = {
    {.compatible = "espressif,esp8266", .data = 0}, {}};

struct esp8266_driver esp8266_drv = {
    .drv =
        {
            .name = "esp8266-drv",
            .probe = esp8266_probe,
            .remove = esp8266_remove,
            .match_table = esp8266_match_table,
        },
    .boot = esp8266_boot,
    .talk = esp8266_talk,
};

PLATFORM_DRIVER(esp8266_drv);

void wlan_set_boot_mode(struct device *device,
                        enum wlan_driver_boot_mode mode) {
  struct esp8266_driver *drv =
      container_of(device->drv, struct esp8266_driver, drv);
  if (!drv || !drv->boot) {
    dev_err(device, "ESP8266 driver does not support boot");
    return;
  }
  drv->boot(device, mode);
}

int wlan_talk(struct device *device, const char *data, size_t len,
              char *response, size_t response_len) {
  struct esp8266_driver *drv =
      container_of(device->drv, struct esp8266_driver, drv);
  if (!drv || !drv->talk) {
    dev_err(device, "ESP8266 driver does not support talk");
    return -ENOSYS;
  }
  return drv->talk(device, data, len, response, response_len);
}
