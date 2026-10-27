#include "leddrv.h"
#include "core/devicetree.h"
#include "drivers/gpiolib.h"
#include <hardware/gpio.h>
#include <hardware/timer.h>
#include <pico/stdlib.h>
#include <stdlib.h>
#include <sys/errno.h>

struct led_driver_pdata {
  struct led_device_pdata *pdata;
  struct led_controller_device *led_device_devs;
  struct device *led_devs;
  int ledcount;
};

struct led_device_pdata {
  struct gpio_desc *gpio;
  bool default_state;
};

int led_device_probe(struct device *dev) { return 0; }

void led_device_remove(struct device *dev) {
  struct led_device_pdata *pdata = dev->platform_data;
  if (!pdata)
    return;
  gpio_desc_release(dev, pdata->gpio);
}

static void leddrv_set_state(struct device *dev, bool state) {
  struct led_device_pdata *pdata = dev->platform_data;
  if (!pdata || !pdata->gpio) {
    dev_err(dev, "LED device %s has no platform data or GPIO descriptor",
            dev->name);
    return;
  }
  gpio_set_value(pdata->gpio, state);
  dev_dbg(dev, "%s: set state to %d", __func__, state);
}

static void leddrv_pulse(struct device *dev, uint32_t duration_ms,
                         uint32_t total_seconds) {
  struct led_device_pdata *pdata = dev->platform_data;
  uint64_t end_time = time_us_64() + (total_seconds * 1000000ULL);
  while (time_us_64() < end_time) {
    leddrv_set_state(dev, true);
    sleep_ms(duration_ms);
    leddrv_set_state(dev, false);
    sleep_ms(duration_ms);
  }
  dev_dbg(dev, "%s: pulsed for %u seconds with %u ms duration", __func__,
          total_seconds, duration_ms);
}

static struct led_device_driver pico_led_driver = {
    .drv =
        (struct driver){
            .name = "pico-led-drv",
            .probe = led_device_probe,
            .remove = led_device_remove,
        },
    .set_state = leddrv_set_state,
    .pulse = leddrv_pulse,
};

PLATFORM_DRIVER(pico_led_driver);

static void noop_free(struct device *dev) {
  // Do nothing, as the device struct is part of the led_driver_pdata
}

static int led_controller_probe(struct device *dev) {
  int ret, ledcount = 0, err = -EINVAL, i = 0;

  fdt_for_each_subnode(ret, dev->fdt, dev->fdt_node_offset) { ledcount++; }

  struct led_driver_pdata *drvdata = calloc(sizeof(*drvdata), 1);
  if (!drvdata) {
    pr_err("OF: Failed to allocate memory for led_driver_pdata");
    return -ENOMEM;
  }
  drvdata->pdata = calloc(ledcount, sizeof(*drvdata->pdata));
  drvdata->led_device_devs =
      calloc(sizeof(*drvdata->led_device_devs), ledcount);
  drvdata->led_devs = calloc(sizeof(*drvdata->led_devs), ledcount);
  if (!drvdata->led_devs || !drvdata->pdata || !drvdata->led_device_devs) {
    pr_err("OF: Failed to allocate memory for led_platform_data");
    err = -ENOMEM;
    goto error_free;
    return -ENOMEM;
  }

  fdt_for_each_subnode(ret, dev->fdt, dev->fdt_node_offset) {
    struct led_device_pdata *pdata = &drvdata->pdata[i];
    struct led_controller_device *led_device_dev = &drvdata->led_device_devs[i];
    struct device *led_dev = &drvdata->led_devs[i];

    const char *name = fdt_get_name(dev->fdt, ret, NULL);
    if (!name) {
      pr_err("OF: Failed to get name for device at node offset %d",
             dev->fdt_node_offset);
      goto error_free;
    }

    struct gpio_desc *gpio_dev = gpio_request(dev, ret, "gpios");
    if (!gpio_dev) {
      pr_err("OF: Failed to request GPIO for device %s", dev->name);
      goto error_free;
    }
    pdata->gpio = gpio_dev;

    int string_len;
    const char *default_state =
        fdt_getprop(dev->fdt, ret, "default-state", &string_len);
    if (default_state) {
      if (strncmp(default_state, "on", string_len) == 0) {
        pdata->default_state = true;
      } else if (strncmp(default_state, "off", string_len) == 0) {
        pdata->default_state = false;
      } else {
        pr_warn("OF: Unknown default-state '%.*s' for device %s", string_len,
                default_state, dev->name);
      }
    }

    led_device_dev->dev = led_dev;
    led_dev->platform_data = pdata;
    led_dev->parent = dev;
    led_dev->fdt = dev->fdt;
    led_dev->fdt_node_offset = ret;
    led_dev->name = name;
    led_dev->id = i;
    led_dev->drv = &pico_led_driver.drv;
    led_dev->free = noop_free;

    if (!device_register(led_device_dev->dev)) {
      pr_err("OF: Failed to register device %s", led_device_dev->dev->name);
      gpio_desc_release(dev, pdata->gpio);
      pdata->gpio = NULL;
      err = -EIO;
      goto error_free;
    }
    i++;
  }
  dev->platform_data = drvdata;
  drvdata->ledcount = ledcount;
  pr_info("OF: Registered %d LED devices for device %s", ledcount, dev->name);
  return 0;
error_free:

  // Deregister any devices that were successfully registered before the error
  for (int j = 0; j < i; j++) {
    struct led_controller_device *led_device_dev = &drvdata->led_device_devs[j];
    if (!device_unregister(led_device_dev->dev)) {
      pr_warn("OF: Failed to unregister device %s:%d",
              led_device_dev->dev->name, led_device_dev->dev->id);
    }
  }
  dev->platform_data = NULL;
  free(drvdata->pdata);
  free(drvdata->led_device_devs);
  free(drvdata->led_devs);
  free(drvdata);
  return err;
}

static void led_controller_remove(struct device *dev) {
  struct led_driver_pdata *pdata = dev->platform_data;
  if (!pdata)
    return;

  dev_dbg(dev, "%s: deinitializing all pins", __func__);
  for (int i = 0; i < pdata->ledcount; i++) {
    struct led_controller_device *led_device_dev = &pdata->led_device_devs[i];
    if (!device_unregister(led_device_dev->dev)) {
      pr_warn("OF: Failed to unregister device %s:%d",
              led_device_dev->dev->name, led_device_dev->dev->id);
    }
  }
  free(pdata->pdata);
  free(pdata->led_device_devs);
  free(pdata->led_devs);
  free(dev->platform_data);
  dev->platform_data = NULL;
  dev_dbg(dev, "%s: deinitialized all pins", __func__);
}

static const struct driver_fdt_match_id led_controller_of_match[] = {
    {.compatible = "gpio-leds", .data = 0}, {}};

static struct led_controller_driver pico_led_controller_driver = {
    .drv =
        (struct driver){
            .name = "pico-led-controller-drv",
            .probe = led_controller_probe,
            .remove = led_controller_remove,
            .match_table = led_controller_of_match,
        },
};
PLATFORM_DRIVER(pico_led_controller_driver);

void led_set_state(struct device *dev, bool state) {
  struct led_device_driver *drv =
      container_of(dev->drv, struct led_device_driver, drv);
  if (!drv || !drv->set_state) {
    dev_err(dev, "LED controller driver does not support set_state");
    return;
  }
  drv->set_state(dev, state);
}

void led_pulse(struct device *dev, uint32_t duration_ms,
               uint32_t total_seconds) {
  struct led_device_driver *drv =
      container_of(dev->drv, struct led_device_driver, drv);
  if (!drv || !drv->pulse) {
    dev_err(dev, "LED controller driver does not support pulse");
    return;
  }
  drv->pulse(dev, duration_ms, total_seconds);
}