#include "latched-switch.h"
#include "core/devicetree.h"
#include "core/irq.h"
#include "drivers/gpiolib.h"
#include <errno.h>

int latched_switch_get_state(struct device *dev, bool *state) {
  struct latched_switch_driver *drv = (struct latched_switch_driver *)dev->drv;
  if (!drv->get_state) {
    pr_err("latched_switch_get_state: get_state function not implemented");
    return -ENOSYS;
  }
  return drv->get_state(dev, state);
}

int latched_switch_set_on_change_callback(
    struct device *dev, latched_switch_on_change_callback_t callback,
    void *data) {
  struct latched_switch_driver *drv = (struct latched_switch_driver *)dev->drv;
  if (!drv->set_on_change_callback) {
    pr_err("latched_switch_set_on_change_callback: set_on_change_callback "
           "function not implemented");
    return -ENOSYS;
  }
  return drv->set_on_change_callback(dev, callback, data);
}

struct latched_switch_device_pdata {
  struct gpio_desc *gpio;
  enum : int {
    PULLUP_NONE,
    PULLUP_INTERNAL,
    PULLDOWN_INTERNAL,
  } mode;
  latched_switch_on_change_callback_t on_change_callback;
  void *on_change_callback_data;
  int irq;
};

static irqreturn_t latched_switch_irq_handler(int irq, irq_event_t event,
                                              void *drvdata) {
  struct device *dev = (struct device *)drvdata;
  struct latched_switch_device_pdata *pdata =
      (struct latched_switch_device_pdata *)dev->platform_data;

  if (!pdata || !pdata->gpio) {
    pr_err("latched_switch_irq_handler: Invalid platform data or GPIO");
    return IRQ_NONE;
  }

  bool new_state;
  int rc = gpio_get_value(pdata->gpio, &new_state, false);
  if (rc != 0) {
    pr_err("latched_switch_irq_handler: Failed to read GPIO value: %d", rc);
    return IRQ_NONE;
  }

  pr_debug("Latched switch state changed to: %d", new_state);

  if (pdata->on_change_callback) {
    pdata->on_change_callback(pdata->on_change_callback_data, new_state);
  }

  return IRQ_HANDLED;
}

static int latched_switch_probe(struct device *dev) {
  struct latched_switch_device_pdata *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    pr_err("Failed to allocate memory for latched_switch_device_pdata");
    return -ENOMEM;
  }
  struct gpio_desc *gpio = gpio_request(dev, dev->fdt_node_offset, "gpios");
  if (!gpio) {
    pr_err("Failed to request GPIO for latched_switch");
    free(pdata);
    return -ENODEV;
  }
  pdata->gpio = gpio;
  int rc = get_prop_int32(dev, "mode", &pdata->mode);
  if (rc != 0) {
    pr_err("Failed to get mode property for latched_switch: %d", rc);
    free(pdata);
    gpio_desc_release(dev, gpio);
    return -EINVAL;
  }
  switch (pdata->mode) {
  case PULLUP_INTERNAL:
    gpio_desc_set_pullup(gpio, true);
    break;
  case PULLDOWN_INTERNAL:
    gpio_desc_set_pulldown(gpio, true);
    break;
  case PULLUP_NONE:
  default:
    break;
  }
  pdata->irq = gpio_to_irq(gpio);
  if (pdata->irq < 0) {
    pr_err("Failed to get IRQ for latched_switch GPIO: %d", pdata->irq);
    free(pdata);
    gpio_desc_release(dev, gpio);
    return pdata->irq;
  }
  dev->platform_data = pdata;
  int irq_rc =
      devm_request_irq(dev, pdata->irq, latched_switch_irq_handler, dev);
  if (irq_rc < 0) {
    pr_err("Failed to request IRQ for latched_switch: %d", irq_rc);
    free(pdata);
    gpio_desc_release(dev, gpio);
    return irq_rc;
  }

  dev_dbg(
      dev,
      "Latched switch initialized on GPIO pin %d with pullup: %d, pulldown: %d",
      gpio->pin, pdata->mode == PULLUP_INTERNAL,
      pdata->mode == PULLDOWN_INTERNAL);
  return 0;
}

static void latched_switch_remove(struct device *dev) {
  struct latched_switch_device_pdata *pdata = dev->platform_data;
  if (pdata) {
    gpio_desc_release(dev, pdata->gpio);
    devm_free_irq(dev, pdata->irq);
    free(pdata);
    dev->platform_data = NULL;
  }
}

static int latched_switch_get_state_impl(struct device *dev, bool *state) {
  struct latched_switch_device_pdata *pdata = dev->platform_data;
  if (!pdata || !pdata->gpio) {
    pr_err("latched_switch_get_state_impl: Invalid platform data or GPIO");
    return -EINVAL;
  }
  return gpio_get_value(pdata->gpio, state, false);
}

static int latched_switch_set_on_change_callback_impl(
    struct device *dev, latched_switch_on_change_callback_t callback,
    void *data) {
  struct latched_switch_device_pdata *pdata = dev->platform_data;
  if (!pdata) {
    pr_err("latched_switch_set_on_change_callback_impl: Invalid platform data");
    return -EINVAL;
  }
  pdata->on_change_callback = callback;
  pdata->on_change_callback_data = data;
  return 0;
}

static const struct driver_fdt_match_id latched_switch_match[] = {
    {.compatible = "royna,latched-switch", .data = NULL}, {}};
static struct latched_switch_driver latched_switch_drv = {
    .drv =
        {
            .name = "latched-switch",
            .probe = latched_switch_probe,
            .remove = latched_switch_remove,
            .match_table = latched_switch_match,
        },
    .get_state = latched_switch_get_state_impl,
    .set_on_change_callback = latched_switch_set_on_change_callback_impl,
};
PLATFORM_DRIVER(latched_switch_drv);
