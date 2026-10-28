#include "gpiolib.h"
#include "core/driver.h"
#include "core/irq.h"
#include "core/printk.h"
#include <errno.h>

#include "core/devicetree.h"
#include "pinctrldrv.h"

struct _Internal_gpiochip_desc {
  int start, num_gpios;
  struct irq_domain *irq_domain;
  struct device *pinctrl_dev;
  struct irq_chip *irq_chip;
  void *drv_data;
};

static int irq_domain_gpiochip_map(struct irq_domain *domain,
                                   struct irq_data *data) {
  if (!domain || !data) {
    pr_err("GPIO: Invalid parameters for irq_domain_gpiochip_map");
    return -EINVAL;
  }
  struct device *dev = domain->master;
  if (!dev || !dev->platform_data) {
    pr_err("GPIO: Invalid device or platform data for irq_domain_gpiochip_map");
    return -EINVAL;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)dev->platform_data;
  if (!chip_desc) {
    pr_err("GPIO: Invalid gpiochip description for device %s", dev->name);
    return -EINVAL;
  }
  data->chip_data = NULL; // No additional chip data for now
  domain->chip = chip_desc->irq_chip;
  return 0;
}

static int irq_domain_gpiochip_unmap(struct irq_domain *domain,
                                     struct irq_data *data) {
  if (!domain || !data) {
    pr_err("GPIO: Invalid parameters for irq_domain_gpiochip_unmap");
    return -EINVAL;
  }
  struct device *dev = domain->master;
  if (!dev || !dev->platform_data) {
    pr_err(
        "GPIO: Invalid device or platform data for irq_domain_gpiochip_unmap");
    return -EINVAL;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)dev->platform_data;
  if (!chip_desc) {
    pr_err("GPIO: Invalid gpiochip description for device %s", dev->name);
    return -EINVAL;
  }
  data->irq = IRQ_NONE;
  return 0;
}

static int irq_domain_gpiochip_xlate(struct irq_domain *domain, int node_offset,
                                     const uint32_t *intspec, int intspec_len,
                                     int *out_hw_irq, irq_event_t *out_event) {
  if (!domain || !intspec || !out_hw_irq || !out_event) {
    pr_err("GPIO: Invalid parameters for irq_domain_gpiochip_xlate");
    return -EINVAL;
  }
  struct device *dev = domain->master;
  if (!dev || !dev->platform_data) {
    pr_err(
        "GPIO: Invalid device or platform data for irq_domain_gpiochip_xlate");
    return -EINVAL;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)dev->platform_data;
  if (!chip_desc) {
    pr_err("GPIO: Invalid gpiochip description for device %s", dev->name);
    return -EINVAL;
  }
  if (intspec_len < 2) {
    pr_err("GPIO: Invalid intspec length for device %s", dev->name);
    return -EINVAL;
  }
  int pin = fdt32_to_cpu(intspec[0]);
  if (pin < chip_desc->start ||
      pin >= chip_desc->start + chip_desc->num_gpios) {
    pr_err("GPIO: Pin %d out of range for device %s", pin, dev->name);
    return -EINVAL;
  }
  *out_hw_irq = pin; // For GPIO implementation, hw_irq is the same as pin
  *out_event = fdt32_to_cpu(intspec[1]);

  // Validate the event type
  if (*out_event & ~(IRQ_EVENT_RISING_EDGE | IRQ_EVENT_FALLING_EDGE |
                     IRQ_EVENT_HIGH_LEVEL | IRQ_EVENT_LOW_LEVEL)) {
    pr_err("GPIO: Invalid IRQ event type %d for device %s", *out_event,
           dev->name);
    return -EINVAL;
  }
#define check_event(event)                                                     \
  if (*out_event & event) {                                                    \
    pr_debug("GPIO: IRQ event type %s for device %s", #event, dev->name);      \
  }
  check_event(IRQ_EVENT_RISING_EDGE);
  check_event(IRQ_EVENT_FALLING_EDGE);
  check_event(IRQ_EVENT_HIGH_LEVEL);
  check_event(IRQ_EVENT_LOW_LEVEL);
#undef check_event
  return 0;
}

struct irq_domain_ops gpiochip_irq_domain_ops = {
    .map = irq_domain_gpiochip_map,
    .unmap = irq_domain_gpiochip_unmap,
    .xlate = irq_domain_gpiochip_xlate,
};

void gpio_desc_release(struct device *request_dev, struct gpio_desc *desc) {
  (void)request_dev; // Unused parameter
  if (!desc || !desc->master) {
    pr_err("GPIO: Invalid gpio_desc or master device");
    return;
  }
  struct gpio_driver *gpio_drv =
      container_of(desc->master->drv, struct gpio_driver, drv);
  if (gpio_drv && gpio_drv->release) {
    gpio_drv->release(request_dev, desc);
  } else {
    free(desc);
  }
}

int gpio_set_value(struct gpio_desc *desc, bool value) {
  if (!desc || !desc->master) {
    pr_err("GPIO: Invalid gpio_desc or master device");
    return -EINVAL;
  }
  struct gpio_driver *gpio_drv =
      container_of(desc->master->drv, struct gpio_driver, drv);
  if (!gpio_drv || !gpio_drv->set_value) {
    pr_err("GPIO: GPIO driver does not support set_value");
    return -ENOSYS;
  }
  return gpio_drv->set_value(desc, value);
}

int gpio_get_value(struct gpio_desc *desc, bool *value, bool raw) {
  if (!desc || !desc->master) {
    pr_err("GPIO: Invalid gpio_desc or master device");
    return -EINVAL;
  }
  struct gpio_driver *gpio_drv =
      container_of(desc->master->drv, struct gpio_driver, drv);
  if (!gpio_drv || !gpio_drv->get_value) {
    pr_err("GPIO: GPIO driver does not support get_value");
    return -ENOSYS;
  }
  int rc = gpio_drv->get_value(desc, value);
  if (rc < 0) {
    return rc;
  }
  if (raw && desc->active_low) {
    *value = !(*value);
  }
  return 0;
}

int gpiochip_register(struct device *dev, const char *name, void *drv_data,
                      struct irq_chip *irq_chip) {
  if (!dev || !name) {
    pr_err("GPIO: Invalid parameters for gpiochip_register");
    return -EINVAL;
  }
  if (!has_property(dev, dev->fdt_node_offset, "gpio-controller")) {
    dev_err(dev, "%s: Device %s is not a GPIO controller", __func__, dev->name);
    return -ENODEV;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      calloc(1, sizeof(struct _Internal_gpiochip_desc));
  if (!chip_desc) {
    pr_err("GPIO: Memory allocation failed for gpiochip_desc");
    return -ENOMEM;
  }
  int ranges[4];
  int ret = get_prop_int32_array(dev, "gpio-ranges", ranges, 4);
  if (ret < 0) {
    dev_err(dev, "Failed to get gpio-ranges property for device %s", dev->name);
    free(chip_desc);
    return ret;
  }
  int start_gpio = ranges[1];
  int num_gpios = ranges[3];
  struct device *pinctrl_dev = device_find_by_phandle(dev->fdt, ranges[0]);
  if (!pinctrl_dev) {
    dev_err(dev, "Failed to find pinctrl device for device %s", dev->name);
    free(chip_desc);
    return -ENODEV;
  }
  dev_dbg(dev, "pinctrl device for GPIO chip %s is %s", dev->name,
          pinctrl_dev->name);
  chip_desc->pinctrl_dev = pinctrl_dev;
  if (has_prop(dev, "interrupt-controller")) {
    dev_dbg(dev,
            "GPIO: Device %s has interrupt-controller property, creating IRQ "
            "domain",
            dev->name);
    chip_desc->irq_domain =
        irq_domain_create(dev, 0, num_gpios, &gpiochip_irq_domain_ops);
    if (!chip_desc->irq_domain) {
      pr_err("GPIO: Failed to create IRQ domain for device %s", dev->name);
      free(chip_desc);
      return -ENOMEM;
    }
  }

  chip_desc->start = start_gpio;
  chip_desc->num_gpios = num_gpios;
  irq_chip->master = dev;
  chip_desc->irq_chip = irq_chip;
  chip_desc->drv_data = drv_data;
  dev->platform_data = chip_desc;
  dev_dbg(dev, "Registered GPIO chip '%s' (%d-%d)", name, start_gpio,
          start_gpio + num_gpios - 1);
  return 0;
}

int gpiochip_unregister(struct device *dev) {
  if (!dev || !dev->platform_data) {
    pr_err("GPIO: Invalid device or platform data for gpiochip_unregister");
    return -EINVAL;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)dev->platform_data;
  struct list_head *pos, *tmp;
  list_for_each_safe(pos, tmp, &chip_desc->irq_domain->map_list) {
    struct irq_data *data = container_of(pos, struct irq_data, list);
    if (chip_desc->irq_domain->ops && chip_desc->irq_domain->ops->unmap) {
      chip_desc->irq_domain->ops->unmap(chip_desc->irq_domain, data);
    }
    struct irq_chip *chip = chip_desc->irq_domain->chip;
    if (chip && chip->free) {
      chip->free(chip, data);
    }

    list_del(&data->list);
    free(data);
  }
  if (chip_desc->irq_domain) {
    irq_domain_destroy(chip_desc->irq_domain);
  }
  free(chip_desc);
  dev->platform_data = NULL;
  return 0;
}

int gpio_to_irq(struct gpio_desc *desc) {
  if (!desc || !desc->master) {
    pr_err("GPIO: Invalid gpio_desc or master device");
    return -EINVAL;
  }
  struct gpio_driver *gpio_drv =
      container_of(desc->master->drv, struct gpio_driver, drv);
  if (!gpio_drv) {
    pr_err("GPIO: Invalid gpio driver for device %s", desc->master->name);
    return -EINVAL;
  }
  if (!desc->master || !desc->master->platform_data) {
    pr_err("GPIO: Invalid master device or platform data for gpio_to_irq");
    return -EINVAL;
  }
  int num_gpios = 0;
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)desc->master->platform_data;
  if (!chip_desc) {
    pr_err("GPIO: Invalid gpiochip description for device %s",
           desc->master->name);
    return -EINVAL;
  }
  num_gpios = chip_desc->num_gpios;
  if (desc->pin < 0 || desc->pin >= num_gpios) {
    pr_err("GPIO: Invalid pin number %d for device %s", desc->pin,
           desc->master->name);
    return -EINVAL;
  }
  struct irq_data *irq_data =
      irq_domain_find_mapping(chip_desc->irq_domain, desc->pin);
  if (IS_ERR(irq_data)) {
    pr_err("GPIO: No IRQ mapping found for GPIO pin %d on device %s", desc->pin,
           desc->master->name);
    return -EINVAL;
  }
  return irq_data->irq;
}

int gpiochip_request_check(struct device *dev, struct device *requester,
                           struct gpio_desc *pin) {
  if (!dev || !dev->platform_data) {
    pr_err("GPIO: Invalid device or platform data for gpiochip_is_in_use");
    return -EINVAL;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)dev->platform_data;
  if (!chip_desc) {
    pr_err("GPIO: Invalid gpiochip description for device %s", dev->name);
    return -EINVAL;
  }
  struct pinctrl_driver *pinctrl_drv = NULL;
  if (chip_desc->pinctrl_dev && chip_desc->pinctrl_dev->drv) {
    pinctrl_drv =
        container_of(chip_desc->pinctrl_dev->drv, struct pinctrl_driver, drv);
  }
  if (pinctrl_drv) {
    return pinctrl_drv->pinctrl_gpio_request(chip_desc->pinctrl_dev, requester,
                                             pin);
  }
  return -ENODEV;
}

int gpiochip_get_driver_data(struct device *dev, void **out_drv_data) {
  if (!dev || !dev->platform_data) {
    pr_err(
        "GPIO: Invalid device or platform data for gpiochip_get_driver_data");
    return -EINVAL;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)dev->platform_data;
  if (!chip_desc) {
    pr_err("GPIO: Invalid gpiochip description for device %s", dev->name);
    return -EINVAL;
  }
  *out_drv_data = chip_desc->drv_data;
  return 0;
}

int gpiochip_release_pin(struct device *dev, struct device *requester,
                         struct gpio_desc *pin) {
  if (!dev || !dev->platform_data) {
    pr_err("GPIO: Invalid device or platform data for gpiochip_release_pin");
    return -EINVAL;
  }
  struct _Internal_gpiochip_desc *chip_desc =
      (struct _Internal_gpiochip_desc *)dev->platform_data;
  if (!chip_desc) {
    pr_err("GPIO: Invalid gpiochip description for device %s", dev->name);
    return -EINVAL;
  }
  struct pinctrl_driver *pinctrl_drv = NULL;
  if (chip_desc->pinctrl_dev && chip_desc->pinctrl_dev->drv) {
    pinctrl_drv =
        container_of(chip_desc->pinctrl_dev->drv, struct pinctrl_driver, drv);
  }
  if (pinctrl_drv && pinctrl_drv->pinctrl_gpio_release) {
    return pinctrl_drv->pinctrl_gpio_release(chip_desc->pinctrl_dev, requester,
                                             pin);
  }
  return -ENODEV;
}