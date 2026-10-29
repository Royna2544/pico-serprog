#include "signal-action.h"
#include "core/devicetree.h"
#include "core/irq.h"
#include "drivers/gpiolib.h"
#include <errno.h>

struct signal_action_device_pdata {
  struct gpio_desc *trig;
  struct gpio_desc *echo;
  int irq;
};

static irqreturn_t signal_action_irq_handler(int irq, irq_event_t event,
                                             void *drvdata) {
  struct device *dev = (struct device *)drvdata;
  struct signal_action_device_pdata *pdata =
      (struct signal_action_device_pdata *)dev->platform_data;

  if (!pdata || !pdata->trig || !pdata->echo) {
    dev_err(dev, "Invalid platform data in IRQ handler");
    return IRQ_NONE;
  }

  // Read the current state of the trig pin
  bool trig_value;
  if (gpio_get_value(pdata->trig, &trig_value, false) != 0) {
    dev_err(dev, "Failed to read trig GPIO value in IRQ handler");
    return IRQ_NONE;
  }

  // Set the echo pin to the same state as the trig pin
  if (gpio_set_value(pdata->echo, trig_value) != 0) {
    dev_err(dev, "Failed to set echo GPIO value in IRQ handler");
    return IRQ_NONE;
  }

  dev_dbg(dev, "IRQ handler: Set echo pin %d to %d based on trig pin %d",
          pdata->echo->pin, trig_value, pdata->trig->pin);

  return IRQ_HANDLED;
}

static int signal_action_probe(struct device *dev) {
  struct signal_action_device_pdata *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(dev, "Failed to allocate memory for signal_action_device_pdata");
    return -ENOMEM;
  }

  pdata->trig = gpio_request(dev, dev->fdt_node_offset, "trig-gpios");
  if (!pdata->trig) {
    dev_err(dev, "Failed to request trig GPIO for signal_action");
    free(pdata);
    return -ENODEV;
  }

  pdata->echo = gpio_request(dev, dev->fdt_node_offset, "echo-gpios");
  if (!pdata->echo) {
    dev_err(dev, "Failed to request echo GPIO for signal_action");
    gpio_desc_release(dev, pdata->trig);
    free(pdata);
    return -ENODEV;
  }

  pdata->irq = gpio_to_irq(pdata->trig);
  if (pdata->irq < 0) {
    dev_err(dev, "Failed to get IRQ for trigger GPIO");
    gpio_desc_release(dev, pdata->trig);
    gpio_desc_release(dev, pdata->echo);
    free(pdata);
    return pdata->irq;
  }

  // Read current trig pin and trigger echo
  bool trig_value;
  if (gpio_get_value(pdata->trig, &trig_value, false) == 0) {
    if (trig_value) {
      dev_dbg(dev, "Initial trig pin is active, setting echo pin high");
      // If trig pin is high, set echo pin high as well
      gpio_set_value(pdata->echo, trig_value);
    }
    bool raw_value;
    gpio_get_value(pdata->trig, &raw_value, true);
    dev_dbg(dev, "Initial trig pin value: %d, raw value: %d", trig_value,
            raw_value);
  }

  int ret = devm_request_irq(dev, pdata->irq, signal_action_irq_handler, dev);
  if (ret < 0) {
    dev_err(dev, "Failed to request IRQ %d for echo GPIO: %d", pdata->irq, ret);
    gpio_desc_release(dev, pdata->trig);
    gpio_desc_release(dev, pdata->echo);
    free(pdata);
    return ret;
  }

  dev_dbg(dev, "Signal action initialized with trig pin %d and echo pin %d",
          pdata->trig->pin, pdata->echo->pin);

  dev->platform_data = pdata;
  return 0;
}

static void signal_action_remove(struct device *dev) {
  struct signal_action_device_pdata *pdata =
      (struct signal_action_device_pdata *)dev->platform_data;
  if (pdata) {
    devm_free_irq(dev, pdata->irq);
    if (pdata->trig)
      gpio_desc_release(dev, pdata->trig);
    if (pdata->echo)
      gpio_desc_release(dev, pdata->echo);
    free(pdata);
  }
}

static const struct driver_fdt_match_id signal_action_match[] = {
    {.compatible = "royna,signal-action-gpio", .data = NULL}, {}};
static struct signal_action_driver signal_action_drv = {
    .drv =
        {
            .name = "signal-action",
            .probe = signal_action_probe,
            .remove = signal_action_remove,
            .match_table = signal_action_match,
        },
};
PLATFORM_DRIVER(signal_action_drv);