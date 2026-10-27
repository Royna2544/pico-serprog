/* Exercise production IRQ/GPIO/pinctrl paths with only hardware/DT stubs. */
#include <assert.h>
#include <string.h>
#include "core/irq.c"
#include "drivers/gpiolib.c"
#include "drivers/gpiodrv.c"
#include "drivers/pinctrldrv.c"

void printk(int level, const char *format, ...) {}
bool driver_register(struct driver *drv) { return true; }
bool has_property(int node, const char *name) { return false; }
bool has_prop(struct device *dev, const char *name) { return false; }
int get_prop_int32_array(struct device *dev, const char *name, int *out, int n) {
  return -ENOENT;
}
struct device *device_find_by_phandle(const void *fdt, int phandle) { return NULL; }
int board_gpio(const char *board, unsigned pin, const char *op, const char *value) {
  return 0;
}
void irq_set_exclusive_handler(uint irq, irq_handler_t handler) {}
void irq_set_enabled(uint irq, bool enabled) {}
void gpio_set_irq_enabled(uint pin, uint32_t events, bool enabled) {}
static uint32_t pending[30], acknowledged[30];
uint32_t gpio_get_irq_event_mask(uint pin) { return pending[pin]; }
void gpio_acknowledge_irq(uint pin, uint32_t events) {
  acknowledged[pin] |= events;
  pending[pin] &= ~events;
}

static struct irq_domain *domain;
static int calls[30], delivered[30], disabled, exited, fail_type, fail_enable;
static bool dispatch_at_enable, check_disable_order;
static int chip_init(struct irq_chip *chip) { return 0; }
static int chip_exit(struct irq_chip *chip) { exited++; return 0; }
static int chip_type(struct irq_chip *chip, struct irq_data *data, irq_event_t event) {
  return fail_type ? -EIO : 0;
}
static int chip_enable(struct irq_chip *chip, struct irq_data *data) {
  if (dispatch_at_enable)
    assert(generic_irq_handle(domain, data->irq, IRQ_EVENT_RISING_EDGE) == 0);
  return fail_enable ? -EIO : 0;
}
static int chip_disable(struct irq_chip *chip, struct irq_data *data) {
  if (check_disable_order)
    assert(generic_irq_handle(domain, data->irq, IRQ_EVENT_RISING_EDGE) == 0);
  disabled++;
  return 0;
}
static irqreturn_t handle(int irq, irq_event_t event, void *arg) {
  int pin = *(int *)arg;
  calls[pin]++;
  delivered[pin] = event;
  return IRQ_HANDLED;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  struct device owner = {.name = "owner"}, controller = {.name = "controller"};
  if (!strcmp(argv[1], "claims")) {
    struct pinctrl_drv_pdata pdata = {0};
    controller.drv = &pinctrl_driver.drv;
    controller.platform_data = &pdata;
    struct _Internal_gpiochip_desc chip = {.pinctrl_dev = &controller};
    struct device gpio = {.name = "gpio", .drv = &gpiodrv.drv,
                          .platform_data = &chip};
    fdt32_t spec[] = {0, cpu_to_fdt32(24), 0};
    struct gpio_desc *a = gpiodrv_request(&gpio, &owner, sizeof(spec), spec);
    spec[1] = cpu_to_fdt32(25);
    struct gpio_desc *b = gpiodrv_request(&gpio, &owner, sizeof(spec), spec);
    assert(a && b && pdata.num_mappings == 2);
    assert(pinctrldrv_request_state(&controller, &owner, "missing") == -ENOENT);
    gpio_desc_release(&owner, a);
    assert(pdata.num_mappings == 1 && pdata.mappings[0].pins[0] == 25);
    a = gpiodrv_request(&gpio, &owner, sizeof(spec), spec);
    assert(!a); // The remaining claim still owns GPIO25.
    gpio_desc_release(&owner, b);
    assert(pdata.num_mappings == 0);
    a = gpiodrv_request(&gpio, &owner, sizeof(spec), spec);
    assert(a);
    gpio_desc_release(&owner, a);
    assert(pdata.num_mappings == 0);
    free(pdata.mappings);
    return 0;
  }

  struct irq_chip chip = {.master = &controller, .init = chip_init,
    .exit = chip_exit, .set_type = chip_type, .enable = chip_enable,
    .disable = chip_disable};
  domain = irq_domain_create(&controller, 0, 30, NULL);
  assert(domain);
  domain->chip = &chip;
  g_irq_domain = domain;
  int pin = 22, other = 23;
  int irq = irq_domain_create_mapping(domain, pin);
  struct irq_data *mapping = irq_domain_find_mapping(domain, pin);
  mapping->event = IRQ_EVENT_BOTH_EDGES;
  mapping->chip_data = &pin;
  dispatch_at_enable = !strcmp(argv[1], "publication");
  fail_type = !strcmp(argv[1], "type-failure");
  fail_enable = !strcmp(argv[1], "enable-failure");
  int rc = devm_request_irq(&owner, irq, handle, &pin);
  if (fail_type || fail_enable) {
    assert(rc == -EIO && chip.refcount == 0 && exited == 1);
    assert(list_empty(&irq_node_list));
  } else {
    assert(rc == 0);
    if (dispatch_at_enable)
      assert(calls[pin] == 1);
    if (!strcmp(argv[1], "events")) {
      int other_irq = irq_domain_create_mapping(domain, other);
      struct irq_data *data = irq_domain_find_mapping(domain, other);
      data->event = IRQ_EVENT_RISING_EDGE;
      data->chip_data = &other;
      assert(devm_request_irq(&owner, other_irq, handle, &other) == 0);
      const uint32_t raw[] = {GPIO_IRQ_EDGE_RISE, GPIO_IRQ_EDGE_FALL,
                             GPIO_IRQ_LEVEL_HIGH, GPIO_IRQ_LEVEL_LOW};
      const irq_event_t expected[] = {IRQ_EVENT_RISING_EDGE, IRQ_EVENT_FALLING_EDGE,
                                     IRQ_EVENT_HIGH_LEVEL, IRQ_EVENT_LOW_LEVEL};
      for (int i = 0; i < 4; i++) {
        pending[pin] = raw[i];
        gpio_irq_handler();
        assert(delivered[pin] == expected[i]);
        assert(calls[pin] == i + 1 && calls[other] == 0 && pending[pin] == 0);
      }
      assert(acknowledged[pin] == 15 && acknowledged[other] == 0);
      devm_free_irq(&owner, other_irq);
    }
    check_disable_order = true;
    devm_free_irq(&owner, irq);
    assert(disabled >= 1 && chip.refcount == 0 && exited == 1);
  }
  assert(irq_domain_create_mapping(domain, -1) == -EINVAL);
  irq_domain_destroy(domain);
  g_irq_domain = NULL;
  return 0;
}
