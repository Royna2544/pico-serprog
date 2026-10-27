#include "pinctrldrv.h"

#include <hardware/gpio.h>
#include <libfdt.h>
#include <stdio.h>
#include <sys/errno.h>

struct pinctrl_mapping {
  const char *function;
  const char *state_name;
  int *pins;
  int num_pins;

  const struct device *used_by; // Track which device is using this mapping
};

struct pinctrl_drv_pdata {
  struct pinctrl_mapping *mappings;
  int num_mappings;
};

static int pinctrldrv_probe(struct device *dev) {
  int i = 0, count = 0, rc = 0;
  fdt_for_each_subnode(i, dev->fdt, dev->fdt_node_offset) { count++; }
  struct pinctrl_drv_pdata *pdata = calloc(1, sizeof(struct pinctrl_drv_pdata));
  if (!pdata) {
    dev_err(dev, "Failed to allocate pinctrl driver private data");
    return -ENOMEM;
  }
  struct pinctrl_mapping *mappings =
      calloc(count, sizeof(struct pinctrl_mapping));
  if (!mappings) {
    dev_err(dev, "Failed to allocate pinctrl mappings");
    free(pdata);
    return -ENOMEM;
  }
  pdata->mappings = mappings;
  pdata->num_mappings = count;
  count = 0;
  rc = -EINVAL;
  fdt_for_each_subnode(i, dev->fdt, dev->fdt_node_offset) {
    const char *config_name = fdt_get_name(dev->fdt, i, NULL);
    if (!config_name) {
      dev_err(dev, "Failed to get name for pinctrl config at node offset %d",
              i);
      goto err;
    }
    dev_info(dev, "Pinctrl config %d: %s", count, config_name);
    const char *function = fdt_getprop(dev->fdt, i, "function", NULL);
    if (!function) {
      dev_err(dev, "Failed to get function: %s", fdt_strerror(i));
      goto err;
    }

    dev_dbg(dev, "Pinctrl config %d function: %s", count, function);

    int str_count = fdt_stringlist_count(dev->fdt, i, "pins");

    if (str_count < 0) {
      dev_err(dev, "Failed to read pins: %s", fdt_strerror(str_count));
      goto err;
    }

    dev_dbg(dev, "Pinctrl config %d has %d pins", count, str_count);
    if (str_count == 0) {
      dev_err(dev, "No pins specified for pinctrl config %s", config_name);
      goto err;
    }

    if (!strcmp(function, "uart") && str_count != 2) {
      dev_err(dev, "UART function requires exactly 2 pins, got %d", str_count);
      goto err;
    } else if (!strcmp(function, "spi") && str_count != 3) {
      dev_err(dev, "SPI function requires exactly 3 pins, got %d", str_count);
      goto err;
    } else if (!strcmp(function, "i2c") && str_count != 2) {
      dev_err(dev, "I2C function requires exactly 2 pins, got %d", str_count);
      goto err;
    }
    pdata->mappings[count].num_pins = str_count;

    for (int j = 0; j < str_count; j++) {
      const char *name = fdt_stringlist_get(dev->fdt, i, "pins", j, NULL);

      if (!name) {
        dev_err(dev, "Failed to get pin name");
        goto err;
      }

      dev_dbg(dev, "pin %d: %s", j, name);
    }

    mappings[count].function = function;
    mappings[count].pins = calloc(str_count, sizeof(int));
    pdata->mappings[count].state_name = config_name;
    if (!mappings[count].pins) {
      dev_err(dev, "Failed to allocate pins: %s", fdt_strerror(i));
      goto err;
    }
    for (int j = 0; j < str_count; j++) {
      const char *name = fdt_stringlist_get(dev->fdt, i, "pins", j, NULL);

      if (!name) {
        dev_err(dev, "Failed to get pin name: %s", fdt_strerror(j));
        goto err;
      }
      if (strstr(name, "gpio") != name) {
        dev_err(dev, "Pin name does not start with 'gpio': %s", name);
        goto err;
      }
      const char *pin_str = name + 4; // Skip "gpio"
      char *endptr;
      long pin_num = strtol(pin_str, &endptr, 10);
      if (*endptr != '\0' || pin_num < 0 || pin_num > 29) {
        dev_err(dev, "Invalid GPIO pin number: %s", name);
        goto err;
      }
      mappings[count].pins[j] = (int)pin_num;
      dev_dbg(dev, "pin %s: %d", name, mappings[count].pins[j]);
    }

    count++;
  }
  dev->platform_data = pdata;
  dev_info(dev, "Successfully probed pinctrl driver with %d mappings", count);
  return 0;
err:
  for (int i = 0; i < count; i++) {
    free(mappings[i].pins);
  }
  free(mappings);
  free(pdata);
  return rc;
}

static void pinctrldrv_remove(struct device *dev) {
  struct pinctrl_drv_pdata *pdata = dev->platform_data;

  for (int i = 0; i < pdata->num_mappings; i++) {
    free(pdata->mappings[i].pins);
  }
  free(pdata->mappings);
  free(pdata);
}

static int pinctrldrv_request_state(struct device *dev,
                                    const struct device *requester,
                                    const char *state_name) {
  struct pinctrl_drv_pdata *pdata = dev->platform_data;
  if (!pdata)
    return -EINVAL;

  struct pinctrl_mapping *requested = NULL;
  for (int i = 0; i < pdata->num_mappings; i++) {
    if (pdata->mappings[i].state_name &&
        strcmp(pdata->mappings[i].state_name, state_name) == 0) {
      if (pdata->mappings[i].used_by) {
        if (pdata->mappings[i].used_by != requester) {
          dev_err(dev, "Pinctrl state %s is already in use by device %s",
                  state_name, pdata->mappings[i].used_by->name);
          return -EBUSY;
        } else {
          return -EALREADY; // Already requested by this device
        }
      }
      requested = &pdata->mappings[i];
      break;
    }
  }
  if (!requested) {
    goto notfound_err;
  }

  for (int i = 0; i < pdata->num_mappings; i++) {
    struct pinctrl_mapping *checking_mapping = &pdata->mappings[i];
    // Now, that exact mapping's ownership is checked above, but we should
    // also check if any of the pins in this mapping are already used by
    // another device.
    if (pdata->mappings[i].used_by) {
      for (int j = 0; j < checking_mapping->num_pins; j++) {
        int pin = checking_mapping->pins[j];
        for (int k = 0; k < requested->num_pins; k++) {
          if (pin == requested->pins[k]) {
            dev_err(dev,
                    "Pinctrl state %s pin %d is already in use by device %s",
                    state_name, pin, checking_mapping->used_by->name);
            return -EBUSY;
          }
        }
      }
    }
  }

  dev_info(dev, "Requesting pinctrl state: %s", state_name);
  if (!strcmp(requested->function, "uart")) {
    gpio_set_function(requested->pins[0], GPIO_FUNC_UART);
    gpio_set_function(requested->pins[1], GPIO_FUNC_UART);
  } else if (!strcmp(requested->function, "spi")) {
    gpio_set_function(requested->pins[0], GPIO_FUNC_SPI);
    gpio_set_function(requested->pins[1], GPIO_FUNC_SPI);
    gpio_set_function(requested->pins[2], GPIO_FUNC_SPI);
    // Leave CS alone as GPIO.
  } else if (!strcmp(requested->function, "i2c")) {
    gpio_set_function(requested->pins[0], GPIO_FUNC_I2C);
    gpio_set_function(requested->pins[1], GPIO_FUNC_I2C);
  }
  // Mark this mapping as used by the device
  requested->used_by = requester;
  return 0;
notfound_err:
  dev_err(dev, "Pinctrl state not found: %s", state_name);
  return -ENOENT;
}

static int pinctrldrv_release_state(struct device *dev,
                                    const struct device *requester,
                                    const char *state_name) {
  struct pinctrl_drv_pdata *pdata = dev->platform_data;
  if (!pdata)
    return -EINVAL;

  dev_info(dev, "Releasing pinctrl state");

  for (int i = 0; i < pdata->num_mappings; i++) {
    if (pdata->mappings[i].state_name &&
        strcmp(pdata->mappings[i].state_name, state_name) == 0) {
      if (pdata->mappings[i].used_by != requester) {
        dev_err(dev, "Pinctrl state %s is not owned by device %s", state_name,
                dev->name);
        return -EINVAL;
      }
      for (int j = 0; j < pdata->mappings[i].num_pins; j++) {
        int pin = pdata->mappings[i].pins[j];
        gpio_set_function(pin, GPIO_FUNC_SIO); // Reset to default function
      }
      // Mark this mapping as no longer used by the device
      pdata->mappings[i].used_by = NULL;
    }
  }
  return 0;
}

int pinctrl_gpio_request(struct device *dev, const struct device *requester,
                         struct gpio_desc *pin) {
  struct pinctrl_drv_pdata *pdata = dev->platform_data;
  if (!pdata)
    return -EINVAL;

  for (int i = 0; i < pdata->num_mappings; i++) {
    struct pinctrl_mapping *mapping = &pdata->mappings[i];
    for (int j = 0; j < mapping->num_pins; j++) {
      if (mapping->pins[j] == pin->pin) {
        dev_err(dev,
                "Pin %d is part of pinctrl mapping %s and cannot be requested "
                "as GPIO",
                pin->pin, mapping->state_name);
        return -EINVAL;
      }
    }
  }

  int *pins = calloc(1, sizeof(int));
  if (!pins) {
    dev_err(dev, "Failed to allocate memory for pinctrl mapping");
    return -ENOMEM;
  }
  // Create a new mapping for this GPIO pin and mark it as used by the requester
  void *tmp = realloc(pdata->mappings, (pdata->num_mappings + 1) *
                                           sizeof(struct pinctrl_mapping));
  if (!tmp) {
    dev_err(dev, "Failed to allocate pinctrl mappings");
    free(pins);
    return -ENOMEM;
  }
  pdata->mappings = tmp;
  struct pinctrl_mapping *new_mapping = &pdata->mappings[pdata->num_mappings];
  new_mapping->function = "gpio";
  new_mapping->state_name = NULL; // No specific state name for GPIO
  new_mapping->pins = pins;
  new_mapping->pins[0] = pin->pin;
  new_mapping->num_pins = 1;
  new_mapping->used_by = requester;
  pdata->num_mappings++;
  dev_dbg(dev, "Pin %d requested as GPIO by device %s", pin->pin,
          requester->name);
  return 0;
}

int pinctrl_gpio_release(struct device *dev, const struct device *requester,
                         struct gpio_desc *pin) {
  struct pinctrl_drv_pdata *pdata = dev->platform_data;
  if (!pdata)
    return -EINVAL;

  for (int i = 0; i < pdata->num_mappings; i++) {
    struct pinctrl_mapping *mapping = &pdata->mappings[i];
    if (mapping->used_by == requester) {
      for (int j = 0; j < mapping->num_pins; j++) {
        if (mapping->pins[j] == pin->pin) {
          // Remove that specific pin from the mapping and free the mapping if
          // it becomes empty
          for (int k = j; k < mapping->num_pins - 1; k++) {
            mapping->pins[k] = mapping->pins[k + 1];
          }
          mapping->num_pins--;
          if (mapping->num_pins == 0) {
            free(mapping->pins);
            free(mapping);
            // Shift remaining mappings down
            for (int k = i; k < pdata->num_mappings - 1; k++) {
              pdata->mappings[k] = pdata->mappings[k + 1];
            }
            pdata->num_mappings--;
          }
          dev_dbg(dev, "Pin %d released as GPIO by device %s", pin->pin,
                  requester->name);
          return 0;
        }
      }
    }
  }

  dev_err(dev, "Pin %d was not requested as GPIO by device %s", pin->pin,
          requester->name);
  return -EINVAL;
}

static const struct driver_fdt_match_id pinctrl_driver_ids[] = {
    {.compatible = "royna,rp2040-pinctrl"},
    {},
};

static struct pinctrl_driver pinctrl_driver = {
    .drv =
        {
            .name = "rp2040-pinctrl",
            .probe = pinctrldrv_probe,
            .remove = pinctrldrv_remove,
            .match_table = pinctrl_driver_ids,
        },
    .request_state = pinctrldrv_request_state,
    .release_state = pinctrldrv_release_state,
    .pinctrl_gpio_request = pinctrl_gpio_request,
    .pinctrl_gpio_release = pinctrl_gpio_release,
};

PLATFORM_DRIVER(pinctrl_driver);