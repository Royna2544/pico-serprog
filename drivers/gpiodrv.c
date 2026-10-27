#include "core/irq.h"
#include "gpiolib.h"

#include "core/devicetree.h"
#include <hardware/gpio.h>
#include <sys/errno.h>

struct gpiodrv_per_pin_irq_data {
  irq_event_t event;
  bool enabled;
};

static void describe_pico_mask(struct device *dev, int pin, int pico_irq_flag) {
  const char *space = " ";
  const char *desc[4];

  if (pico_irq_flag == 0)
    return; // No flags set, nothing to describe
#define push(pico, i)                                                          \
  if (pico_irq_flag & pico)                                                    \
    desc[i] = #pico;                                                           \
  else                                                                         \
    desc[i] = space;
  push(GPIO_IRQ_EDGE_RISE, 0);
  push(GPIO_IRQ_EDGE_FALL, 1);
  push(GPIO_IRQ_LEVEL_HIGH, 2);
  push(GPIO_IRQ_LEVEL_LOW, 3);
#undef push
  if (dev)
    dev_dbg(dev, "GPIO pin %d has pico irq flags: %s %s %s %s (raw: 0x%x)", pin,
            desc[0], desc[1], desc[2], desc[3], pico_irq_flag);
  else
    pr_debug("GPIO pin %d has pico irq flags: %s %s %s %s (raw: 0x%x)", pin,
             desc[0], desc[1], desc[2], desc[3], pico_irq_flag);
}

static int translate_ours_to_pico_irq_flags(irq_event_t event) {
  uint32_t pico_irq_flags = 0;
#define push(our, pico)                                                        \
  if (event & our)                                                             \
    pico_irq_flags |= pico;
  push(IRQ_EVENT_RISING_EDGE, GPIO_IRQ_EDGE_RISE);
  push(IRQ_EVENT_FALLING_EDGE, GPIO_IRQ_EDGE_FALL);
  push(IRQ_EVENT_HIGH_LEVEL, GPIO_IRQ_LEVEL_HIGH);
  push(IRQ_EVENT_LOW_LEVEL, GPIO_IRQ_LEVEL_LOW);
#undef push
  pr_debug("Translated our irq event 0x%x to pico irq flags 0x%x", event,
           pico_irq_flags);
  return pico_irq_flags;
}

static irq_event_t translate_pico_to_ours_irq_flags(uint32_t flags) {
  irq_event_t event = IRQ_EVENT_NONE;
  if (flags & GPIO_IRQ_EDGE_RISE)
    event |= IRQ_EVENT_RISING_EDGE;
  if (flags & GPIO_IRQ_EDGE_FALL)
    event |= IRQ_EVENT_FALLING_EDGE;
  if (flags & GPIO_IRQ_LEVEL_HIGH)
    event |= IRQ_EVENT_HIGH_LEVEL;
  if (flags & GPIO_IRQ_LEVEL_LOW)
    event |= IRQ_EVENT_LOW_LEVEL;
  return event;
}

static struct irq_domain *g_irq_domain = NULL;
static void gpio_irq_handler(void) {
  struct irq_domain *domain = g_irq_domain;
  if (IS_ERR_OR_NULL(domain))
    return;

  pr_debug("GPIO: Handling IRQ for GPIO bank 0");
  struct list_head *pos;
  list_for_each(pos, &domain->map_list) {
    struct irq_data *data = container_of(pos, struct irq_data, list);
    if (!data->chip_data)
      continue;
    int pin = data->hw_irq;
    uint32_t event = gpio_get_irq_event_mask(pin);
    if (!event)
      continue;

    describe_pico_mask(NULL, pin, event);
    // Acknowledge only the snapshot being dispatched, not every configured edge.
    gpio_acknowledge_irq(pin, event);
    int rc = generic_irq_handle(domain, data->irq,
                                translate_pico_to_ours_irq_flags(event));
    if (rc < 0) {
      pr_err("GPIO: Dispatch returned %d.", rc);
    }
  }
  pr_debug("GPIO: Finished handling IRQ for GPIO bank 0");
}

static int gpiodrv_irq_init(struct irq_chip *chip) {
  if (!chip || !chip->master) {
    pr_err("GPIO: Invalid irq_chip or master device for irq_init");
    return -EINVAL;
  }

  g_irq_domain = irq_domain_find_by_master(chip->master);

  if (IS_ERR(g_irq_domain)) {
    pr_err("GPIO: No IRQ domain found for device %s", chip->master->name);
    return PTR_ERR(g_irq_domain);
  }
  irq_set_exclusive_handler(IO_IRQ_BANK0, gpio_irq_handler);
  irq_set_enabled(IO_IRQ_BANK0, true);
  return 0;
}

static int gpiodrv_irq_exit(struct irq_chip *chip) {
  irq_set_enabled(IO_IRQ_BANK0, false);
  g_irq_domain = NULL;
  return 0;
}
static int gpiodrv_irq_enable(struct irq_chip *chip, struct irq_data *data) {
  struct device *dev = data->domain->master;
  if (!dev) {
    pr_err("GPIO: Invalid device or platform data for irq_enable");
    return -EINVAL;
  }
  int pin = data->hw_irq;
  struct gpiodrv_per_pin_irq_data *pin_irq_data =
      (struct gpiodrv_per_pin_irq_data *)data->chip_data;
  if (!pin_irq_data) {
    pr_err("GPIO: No IRQ data for pin %d on device %s", pin, dev->name);
    return -EINVAL;
  }
  pin_irq_data->enabled = true;
  int curr_mask = gpio_get_irq_event_mask(pin);
  if (curr_mask != 0) {
    describe_pico_mask(dev, pin, curr_mask);
    dev_err(dev, "Acknowledge them first before enabling new IRQs for pin %d",
            pin);
    return -EINVAL;
  }
  gpio_set_irq_enabled(
      pin, translate_ours_to_pico_irq_flags(pin_irq_data->event), true);
  dev_dbg(dev, "Enabled IRQ for GPIO pin %d", pin);
  return 0;
}

static int gpiodrv_irq_disable(struct irq_chip *chip, struct irq_data *data) {
  struct device *dev = data->domain->master;
  if (!dev) {
    pr_err("GPIO: Invalid device or platform data for irq_disable");
    return -EINVAL;
  }
  int pin = data->hw_irq;
  struct gpiodrv_per_pin_irq_data *pin_irq_data =
      (struct gpiodrv_per_pin_irq_data *)data->chip_data;
  if (!pin_irq_data) {
    pr_err("GPIO: No IRQ data for pin %d on device %s", pin, dev->name);
    return -EINVAL;
  }
  pin_irq_data->enabled = false;
  gpio_set_irq_enabled(
      pin, translate_ours_to_pico_irq_flags(pin_irq_data->event), false);
  dev_dbg(dev, "Disabled IRQ for GPIO pin %d", pin);
  return 0;
}

static int gpiodrv_irq_ack(struct irq_chip *chip, struct irq_data *data) {
  struct device *dev = data->domain->master;
  if (!dev) {
    pr_err("GPIO: Invalid device or platform data for irq_ack");
    return -EINVAL;
  }
  int pin = data->hw_irq;
  struct gpiodrv_per_pin_irq_data *pin_irq_data =
      (struct gpiodrv_per_pin_irq_data *)data->chip_data;
  if (!pin_irq_data) {
    pr_err("GPIO: No IRQ data for pin %d on device %s", pin, dev->name);
    return -EINVAL;
  }
  gpio_acknowledge_irq(pin,
                       translate_ours_to_pico_irq_flags(pin_irq_data->event));
  dev_dbg(dev, "Acknowledged IRQ for GPIO pin %d", pin);
  return 0;
}

static int gpiodrv_irq_set_type(struct irq_chip *chip, struct irq_data *data,
                                irq_event_t event) {
  if (!data->chip_data) {
    data->chip_data = calloc(1, sizeof(struct gpiodrv_per_pin_irq_data));
    if (!data->chip_data) {
      return -ENOMEM;
    }
  }
  struct gpiodrv_per_pin_irq_data *pin_irq_data =
      (struct gpiodrv_per_pin_irq_data *)data->chip_data;
  pin_irq_data->event = event;
  int pin = data->hw_irq;
  // Translate to pico sdk
  uint32_t pico_irq_flags = 0;
  pico_irq_flags = translate_ours_to_pico_irq_flags(event);
  describe_pico_mask(data->domain->master, pin, pico_irq_flags);
  dev_dbg(data->domain->master,
          "Setting IRQ type for GPIO pin %d to pico IRQ event 0x%x", pin,
          pico_irq_flags);
  gpio_set_irq_enabled(pin, pico_irq_flags, pin_irq_data->enabled);
  return 0;
}

static struct irq_chip gpio_chip = {
    .name = "rp2040-sio-irq",
    .master = NULL, // This will be set during initialization
    .init = gpiodrv_irq_init,
    .exit = gpiodrv_irq_exit,
    .enable = gpiodrv_irq_enable,
    .disable = gpiodrv_irq_disable,
    .ack = gpiodrv_irq_ack,
    .set_type = gpiodrv_irq_set_type,
    .refcount = 0,
};

static int gpiodrv_probe(struct device *dev) {
  return gpiochip_register(dev, "raspberry-pi-sio", NULL, &gpio_chip);
}
static void gpiodrv_remove(struct device *dev) { gpiochip_unregister(dev); }

static int gpiodrv_get_value(struct gpio_desc *desc, bool *value) {
  *value = gpio_get(desc->pin) ^ desc->active_low;
  return 0;
}
static int gpiodrv_set_value(struct gpio_desc *desc, bool value) {
  if (!desc->controlled)
    gpio_init(desc->pin);
  gpio_put(desc->pin, value ^ desc->active_low);
  gpio_set_dir(desc->pin, GPIO_OUT);
  desc->direction = GPIO_DIR_OUT;
  desc->controlled = true;
  return 0;
}
static int gpiodrv_set_direction(struct gpio_desc *desc, gpio_direction_t dir) {
  if (!desc->controlled)
    gpio_init(desc->pin);
  if (dir == GPIO_DIR_IN) {
    gpio_set_dir(desc->pin, GPIO_IN);
  } else {
    gpio_set_dir(desc->pin, GPIO_OUT);
  }
  desc->direction = dir;
  desc->controlled = true;
  return 0;
}
static void gpiodrv_pullup(struct gpio_desc *desc, bool enable) {
  gpio_set_pulls(desc->pin, enable, desc->pulldown);
  desc->pullup = enable;
  desc->controlled = true;
}
static void gpiodrv_pulldown(struct gpio_desc *desc, bool enable) {
  gpio_set_pulls(desc->pin, desc->pullup, enable);
  desc->pulldown = enable;
  desc->controlled = true;
}

static void gpiodrv_reset(struct gpio_desc *desc) {
  if (!desc || !desc->master) {
    pr_err("GPIO: Invalid gpio_desc or master device");
    return;
  }

  gpio_set_dir(desc->pin, GPIO_IN);
  gpio_set_pulls(desc->pin, false, false);
  desc->pullup = false;
  desc->pulldown = false;
  desc->direction = GPIO_DIR_IN;
  desc->controlled = false;
  gpio_put(desc->pin, false);
}

void gpio_reset(struct gpio_desc *desc) {
  struct gpio_driver *gpio_drv =
      container_of(desc->master->drv, struct gpio_driver, drv);
  if (gpio_drv && gpio_drv->reset) {
    gpio_drv->reset(desc);
  } else {
    pr_err("GPIO: GPIO driver does not support reset");
  }
}

static struct gpio_desc *gpiodrv_request(struct device *dev,
                                         struct device *requester, int len,
                                         const fdt32_t *gpio_node_value) {
  if (len < 3 * sizeof(fdt32_t)) {
    dev_err(dev, "GPIO property is too short for device %s", dev->name);
    return NULL;
  }
  uint32_t pin = fdt32_to_cpu(gpio_node_value[1]);
  uint32_t flags = fdt32_to_cpu(gpio_node_value[2]);
  bool active_low = (flags & 0x1) != 0; // Check if active low flag is set

  struct gpio_desc *desc = calloc(1, sizeof(struct gpio_desc));
  if (!desc) {
    return NULL;
  }
  desc->master = dev;
  desc->pin = pin;
  desc->controlled = false;
  desc->pullup = false;
  desc->pulldown = false;
  desc->direction = GPIO_DIR_IN;
  desc->active_low = active_low;

  if (gpiochip_request_check(dev, requester, desc) != 0) {
    dev_err(dev, "GPIO pin %d is already requested by another device", pin);
    free(desc);
    return NULL;
  }

  dev_dbg(dev, "Got GPIO request pin %d (active_low=%d) for device %s", pin,
          active_low, dev->name);
  return desc;
}

static void gpiodrv_release(struct device *request_dev,
                            struct gpio_desc *desc) {
  int rc = gpiochip_release_pin(desc->master, request_dev, desc);
  if (rc)
    dev_warn(desc->master, "Failed to release GPIO pin %d claim: %d",
             desc->pin, rc);
  gpiodrv_reset(desc);
  free(desc);
}

static int gpiodrv_request_irq(struct device *dev, struct device *requester,
                               int len, const fdt32_t *irq_node_value) {
  if (len < 3 * sizeof(fdt32_t)) {
    dev_err(dev, "IRQ property is too short for device %s", dev->name);
    return -EINVAL;
  }
  uint32_t pin = fdt32_to_cpu(irq_node_value[1]);
  uint32_t irq_flags = fdt32_to_cpu(irq_node_value[2]);
  struct gpio_desc desc = {
      .master = dev,
      .pin = pin,
  };

  int irq = gpio_to_irq(&desc);
  if (irq < 0) {
    dev_err(dev, "Failed to map GPIO pin %d to IRQ for device %s", pin,
            dev->name);
    return irq;
  }

  dev_dbg(dev, "GPIO: Mapped GPIO pin %d to IRQ %d for device %s", pin, irq,
          dev->name);
  return irq;
}

int gpiodrv_release_irq(struct device *dev, struct device *requester, int irq) {
  // In this implementation, we don't have a specific release mechanism for IRQs
  // associated with GPIO pins. The IRQ will be released when the GPIO pin is
  // released.
  dev_dbg(dev, "Released IRQ %d for device %s", irq, dev->name);
  return 0;
}

static const struct driver_fdt_match_id gpiodrv_match[] = {
    {.compatible = "royna,rp2040-sio-gpio", .data = NULL}, {}};

static struct gpio_driver gpiodrv = {.drv =
                                         {
                                             .name = "rp2040-sio-gpio",
                                             .probe = gpiodrv_probe,
                                             .remove = gpiodrv_remove,
                                             .match_table = gpiodrv_match,
                                         },
                                     .get_value = gpiodrv_get_value,
                                     .set_value = gpiodrv_set_value,
                                     .set_direction = gpiodrv_set_direction,
                                     .pulldown = gpiodrv_pulldown,
                                     .pullup = gpiodrv_pullup,
                                     .reset = gpiodrv_reset,
                                     .request = gpiodrv_request,
                                     .release = gpiodrv_release,
                                     .request_irq = gpiodrv_request_irq,
                                     .release_irq = gpiodrv_release_irq};

PLATFORM_DRIVER(gpiodrv);
