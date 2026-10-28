#include "devicetree.h"
#include "core/driver.h"
#include "init.h"
#include "panic.h"
#include "printk.h"
#include <libfdt.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/errno.h>

extern const uint8_t board_dtb_start[];
extern const uint8_t board_dtb_end[];

const char *hardware_model = NULL;
static bool registration_failed;

static void add_recursive_devices(const void *fdt, int node_offset,
                                  struct device *parent) {
  const char *compatible = fdt_getprop(fdt, node_offset, "compatible", NULL);
  // Default to parent if no compatible string is found
  struct device *dev = parent;
  bool probe_ok = false;

  if (compatible) {
    if (fdt_getprop(fdt, node_offset, "status", NULL)) {
      const char *status = fdt_getprop(fdt, node_offset, "status", NULL);
      if (strcmp(status, "okay") != 0 && strcmp(status, "ok") != 0) {
        pr_info("OF: Skipping device %s with compatible %s due to status '%s'",
                fdt_get_name(fdt, node_offset, NULL), compatible, status);
        return;
      }
    }

    if (fdt_node_check_compatible(fdt, node_offset, "simple-bus") == 0) {
      pr_info("OF: Skipping simple-bus node %s",
              fdt_get_name(fdt, node_offset, NULL));
      goto child;
    }

    dev = calloc(1, sizeof(*dev));
    if (!dev) {
      pr_err("OF: Failed to allocate memory for device");
      registration_failed = true;
      return;
    }
    const char *name = fdt_get_name(fdt, node_offset, NULL);
    if (!name) {
      pr_err("OF: Failed to get name for device at node offset %d",
             node_offset);
      registration_failed = true;
      free(dev);
      return;
    }
    pr_info("OF: Adding device: %s, compatible: %s", name, compatible);
    dev->name = name;
    dev->fdt = fdt;
    dev->fdt_node_offset = node_offset;
    dev->platform_data = NULL;
    dev->deferred = false;
    dev->parent = parent;
    dev->compatible = compatible;
    if (parent && !parent->drv) {
      pr_warn("OF: Parent device %s has no driver, cannot probe device %s with "
              "compatible %s",
              parent ? parent->name : "(null)", name, compatible);
    }
    dev->drv = driver_find_for_device(dev);

    if ((probe_ok = !device_register(dev))) {
      pr_err("OF: Failed to register device %s", dev->name);
      registration_failed = true;
      free(dev);
      return;
    }
  }

  int node;
child:
  fdt_for_each_subnode(node, fdt, node_offset) {
    add_recursive_devices(fdt, node, dev);
  }
}

static int devicetree_init() {
  const void *fdt = board_dtb_start;
  registration_failed = false;

  int ret = fdt_check_full(fdt, board_dtb_end - board_dtb_start);

  if (ret != 0) {
    do_panic("OF: Device tree validation failed: %s", fdt_strerror(ret));
    return ret;
  }

  const char *model = fdt_getprop(fdt, 0, "model", NULL);
  if (model) {
    pr_info("OF: Device tree model: %s", model);
    hardware_model = model;
  } else {
    pr_warn("OF: Device tree model property not found");
  }

  // First, traverse all compatible nodes and register them as devices.
  fdt_for_each_subnode(ret, fdt, 0) { add_recursive_devices(fdt, ret, NULL); }

  // Second pass: handle /aliases
  int aliases = fdt_path_offset(fdt, "/aliases");
  if (aliases < 0)
    return aliases;

  int prop_off;

  fdt_for_each_property_offset(prop_off, fdt, aliases) {
    int len;

    const struct fdt_property *prop =
        fdt_get_property_by_offset(fdt, prop_off, &len);

    if (!prop)
      continue;

    const char *name = fdt_string(fdt, fdt32_to_cpu(prop->nameoff));
    const char *path = (const char *)prop->data;

    if (!name || len < 2 || path[0] != '/' || path[len - 1] != '\0')
      continue;

    int node = fdt_path_offset(fdt, path);
    if (node < 0) {
      pr_warn("OF: alias %s points to invalid path %s", name, path);
      continue;
    }
    pr_info("OF: alias %s -> %s (offset %d)", name, path, node);

    // Find the device associated with the alias
    struct device *dev = device_find_by_offset(fdt, node);
    if (!dev) {
      pr_warn("OF: No device found for alias %s", name);
      continue;
    }
    pr_info("OF: alias %s -> device %s", name, dev->name);
    dev->alias = name;
  }

  if (ret == -FDT_ERR_NOTFOUND) {
    ret = 0;
  }

  if (registration_failed)
    return -ENODEV;
  return ret;
}
driver_initcall(devicetree_init);

bool has_property(struct device *dev, int node_offset,
                  const char *property_name) {
  const struct fdt_property *prop =
      fdt_get_property(board_dtb_start, node_offset, property_name, NULL);
  return prop != NULL;
}

struct device *find_device_by_prop(struct device *dev, const char *prop_name,
                                   int fdt_offset, int index) {
  if (!dev->fdt) {
    dev_err(dev, "Device %s has no device tree node", dev->name);
    return NULL;
  }
  int len;
  const fdt32_t *phandle = fdt_getprop(dev->fdt, fdt_offset, prop_name, &len);
  if (!phandle) {
    dev_err(dev, "Device %s has no property %s", dev->name, prop_name);
    return NULL;
  }
  int count = len / sizeof(fdt32_t);
  if (index < 0 || index >= count) {
    dev_err(dev, "Index %d out of bounds for property %s with %d entries",
            index, prop_name, count);
    return NULL;
  }
  uint32_t phandle_value = fdt32_to_cpu(phandle[index]);
  int node_offset = fdt_node_offset_by_phandle(dev->fdt, phandle_value);
  if (node_offset < 0) {
    dev_err(dev, "Failed to find node for phandle %u in property %s",
            phandle_value, prop_name);
    return NULL;
  }
  struct device *target_dev = device_find_by_offset(dev->fdt, node_offset);
  if (!target_dev) {
    dev_err(dev,
            "No device found for node offset %d (phandle %u) in property %s",
            node_offset, phandle_value, prop_name);
    return NULL;
  }
  return target_dev;
}

#include "drivers/pinctrldrv.h"

struct pinctrl_find_state_data {
  const char *state_name;
  struct device *pinctrl_dev;
  struct pinctrl_driver *pinctrl_drv;
};

static bool get_pinctrl_driver(const struct device *dev,
                               struct pinctrl_find_state_data *out_data) {
  if (!dev->fdt)
    return false;

  int pinctrl_count =
      fdt_stringlist_count(dev->fdt, dev->fdt_node_offset, "pinctrl-names");
  if (pinctrl_count < 0) {
    dev_dbg(dev, "No pinctrl-names property found");
    return false;
  }

  int wanted_index = -1;
  for (int i = 0; i < pinctrl_count; i++) {
    const char *name = fdt_stringlist_get(dev->fdt, dev->fdt_node_offset,
                                          "pinctrl-names", i, NULL);
    if (name && strcmp(name, "default") == 0) {
      // Found the default pinctrl state
      wanted_index = i;
      break;
    }
  }
  if (wanted_index < 0) {
    dev_dbg(dev, "No default pinctrl state found");
    return false;
  }

  char prop_name[32];
  snprintf(prop_name, sizeof(prop_name), "pinctrl-%d", wanted_index);
  int len;
  const fdt32_t *pinctrl_phandle =
      fdt_getprop(dev->fdt, dev->fdt_node_offset, prop_name, &len);
  if (!pinctrl_phandle || len != sizeof(fdt32_t)) {
    dev_err(dev, "Failed to get pinctrl phandle for default state");
    return false;
  }
  uint32_t phandle = fdt32_to_cpu(*pinctrl_phandle);
  int pinctrl_state_node_offset = fdt_node_offset_by_phandle(dev->fdt, phandle);
  if (pinctrl_state_node_offset < 0) {
    dev_err(dev, "Failed to resolve phandle %u for device %s", phandle,
            dev->name);
    return false;
  }

  const char *name = fdt_get_name(dev->fdt, pinctrl_state_node_offset, NULL);
  if (!name) {
    dev_err(dev, "Failed to get name for pinctrl state at node offset %d",
            pinctrl_state_node_offset);
    return false;
  }
  dev_dbg(dev, "Found referenced pinctrl state: %s", name);

  // Walk up the tree to find the pinctrl device
  int gpio_node_offset = fdt_parent_offset(dev->fdt, pinctrl_state_node_offset);
  if (gpio_node_offset < 0) {
    dev_err(dev, "Failed to find parent node for pinctrl state %s", name);
    return false;
  }

  struct device *pinctrl_dev =
      device_find_by_offset(dev->fdt, gpio_node_offset);
  if (!pinctrl_dev) {
    dev_err(dev, "Failed to get pinctrl device for device %s", dev->name);
    return false;
  }

  if (!pinctrl_dev->drv) {
    dev_err(dev, "pinctrl device %s has no driver bound for device %s",
            pinctrl_dev->name, dev->name);
    return false;
  }

  struct pinctrl_driver *pinctrl_drv =
      container_of(pinctrl_dev->drv, struct pinctrl_driver, drv);
  out_data->state_name = name;
  out_data->pinctrl_dev = pinctrl_dev;
  out_data->pinctrl_drv = pinctrl_drv;
  return true;
}

int apply_pinctrl_default(const struct device *dev) {
  struct pinctrl_find_state_data data;
  if (!get_pinctrl_driver(dev, &data)) {
    dev_dbg(dev, "No pinctrl driver found for device %s", dev->name);
    if (dev->fdt &&
        fdt_getprop(dev->fdt, dev->fdt_node_offset, "pinctrl-names", NULL) &&
        fdt_stringlist_search(dev->fdt, dev->fdt_node_offset, "pinctrl-names",
                              "default") != -FDT_ERR_NOTFOUND)
      return -EINVAL;
    return -ENOENT;
  }
  if (!data.pinctrl_drv->request_state) {
    dev_err(dev,
            "pinctrl driver %s has no request_state function for device %s",
            data.pinctrl_dev->name, dev->name);
    return -ENOSYS;
  }
  pr_debug("Applying pinctrl state %s for device %s using pinctrl device %s",
           data.state_name, dev->name, data.pinctrl_dev->name);
  return data.pinctrl_drv->request_state(data.pinctrl_dev, dev,
                                         data.state_name);
}

int remove_pinctrl_default(const struct device *dev) {
  struct pinctrl_find_state_data data;
  if (!get_pinctrl_driver(dev, &data)) {
    return -ENOENT;
  }
  if (!data.pinctrl_drv->release_state) {
    dev_err(dev,
            "pinctrl driver %s has no release_state function for device %s",
            data.pinctrl_dev->name, dev->name);
    return -ENOSYS;
  }
  return data.pinctrl_drv->release_state(data.pinctrl_dev, dev,
                                         data.state_name);
}

#include "drivers/gpiolib.h"

int get_prop_int32_array(struct device *dev, const char *prop_name,
                         int *out_values, int array_size) {
  if (!dev->fdt) {
    dev_err(dev, "Device %s has no device tree node", dev->name);
    return -ENODEV;
  }

  int len;
  const fdt32_t *prop =
      fdt_getprop(dev->fdt, dev->fdt_node_offset, prop_name, &len);

  if (!prop) {
    dev_err(dev, "Property %s for device %s not found", prop_name, dev->name);
    return -ENOENT;
  }
  int num_elements = len / sizeof(fdt32_t);
  if (num_elements > array_size) {
    dev_err(dev,
            "Property %s for device %s has more elements (%d) than expected "
            "(%d)",
            prop_name, dev->name, num_elements, array_size);
    return -EINVAL;
  } else if (num_elements < array_size) {
    dev_err(dev,
            "Property %s for device %s has fewer elements (%d) than expected "
            "(%d)",
            prop_name, dev->name, num_elements, array_size);
    return -EINVAL;
  }
  for (int i = 0; i < num_elements; i++) {
    out_values[i] = fdt32_to_cpu(prop[i]);
  }
  return num_elements;
}

struct device *get_gpio_device(struct device *dev, int node_offset,
                               const char *name, int *len,
                               const fdt32_t **gpio_node_value) {
  if (!dev->fdt) {
    dev_err(dev, "Device %s has no device tree node", dev->name);
    return NULL;
  }

  *gpio_node_value = fdt_getprop(dev->fdt, node_offset, name, len);
  if (!*gpio_node_value || *len < sizeof(fdt32_t)) {
    dev_err(dev, "Failed to get GPIO property %s for device %s", name,
            dev->name);
    return NULL;
  }
  uint32_t phandle = fdt32_to_cpu(**gpio_node_value);
  if (!phandle) {
    dev_err(dev, "GPIO property %s for device %s has invalid phandle", name,
            dev->name);
    return NULL;
  }
  int gpio_node_offset = fdt_node_offset_by_phandle(dev->fdt, phandle);
  if (gpio_node_offset < 0) {
    dev_err(dev, "Failed to resolve phandle %u for device %s", phandle,
            dev->name);
    return NULL;
  }

  // Get the #gpio-cells property to determine how many cells are expected for
  // this GPIO
  int cells_len;
  const fdt32_t *gpio_cells_prop =
      fdt_getprop(dev->fdt, gpio_node_offset, "#gpio-cells", &cells_len);
  if (!gpio_cells_prop || cells_len != sizeof(fdt32_t)) {
    dev_err(dev, "Failed to get #gpio-cells property for device %s", dev->name);
    return NULL;
  }
  uint32_t gpio_cells = fdt32_to_cpu(*gpio_cells_prop);
  if (gpio_cells != *len / sizeof(fdt32_t) - 1) {
    dev_err(dev, "Mismatched #gpio-cells value %u for device %s", gpio_cells,
            dev->name);
    return NULL;
  }

  struct device *gpio_dev = device_find_by_offset(dev->fdt, gpio_node_offset);
  if (!gpio_dev) {
    dev_err(dev, "Failed to find GPIO device for device %s", dev->name);
    return NULL;
  }

  if (!gpio_dev->drv) {
    dev_err(dev, "GPIO device %s has no driver bound for device %s",
            gpio_dev->name, dev->name);
    return NULL;
  }

  return gpio_dev;
}

struct gpio_desc *gpio_request(struct device *dev, int node_offset,
                               const char *name) {
  int len;
  const fdt32_t *gpio_node_value;
  struct device *gpio_dev =
      get_gpio_device(dev, node_offset, name, &len, &gpio_node_value);
  if (!gpio_dev) {
    dev_err(dev, "Failed to get GPIO device for device %s", dev->name);
    return NULL;
  }
  struct gpio_driver *gpio_drv =
      container_of(gpio_dev->drv, struct gpio_driver, drv);
  if (!gpio_drv->request) {
    return NULL;
  }
  struct gpio_desc *desc =
      gpio_drv->request(gpio_dev, dev, len, gpio_node_value);
  if (!desc) {
    dev_err(dev, "Failed to request GPIO %s for device %s", name, dev->name);
    return NULL;
  }
  dev_dbg(dev, "Requested GPIO %s for device %s", name, dev->name);
  return desc;
}

int gpio_request_irq(struct device *dev, struct device *requester,
                     int node_offset, const char *name) {
  int len;
  const fdt32_t *irq_node_value;
  struct device *gpio_dev =
      get_gpio_device(dev, node_offset, name, &len, &irq_node_value);
  if (!gpio_dev) {
    dev_err(dev, "Failed to get GPIO device for device %s", dev->name);
    return -EINVAL;
  }
  struct gpio_driver *gpio_drv =
      container_of(gpio_dev->drv, struct gpio_driver, drv);
  if (!gpio_drv->request_irq) {
    return -ENOSYS;
  }
  int irq = gpio_drv->request_irq(gpio_dev, requester, len, irq_node_value);
  if (irq < 0) {
    dev_err(dev, "Failed to request IRQ for device %s", dev->name);
    return irq;
  }
  return irq;
}

static int get_parent_size_rules(struct device *dev, int *address_cells,
                                 int *size_cells) {
  int parent_offset = fdt_parent_offset(dev->fdt, dev->fdt_node_offset);
  if (parent_offset < 0) {
    dev_err(dev, "Failed to get parent node for device %s", dev->name);
    return -EINVAL;
  }
  int len;
  const fdt32_t *address_cells_prop =
      fdt_getprop(dev->fdt, parent_offset, "#address-cells", &len);
  if (!address_cells_prop || len != sizeof(fdt32_t)) {
    dev_err(dev, "Failed to get #address-cells property for device %s",
            dev->name);
    return -EINVAL;
  }
  const fdt32_t *size_cells_prop =
      fdt_getprop(dev->fdt, parent_offset, "#size-cells", &len);
  if (!size_cells_prop || len != sizeof(fdt32_t)) {
    dev_err(dev, "Failed to get #size-cells property for device %s", dev->name);
    return -EINVAL;
  }
  *address_cells = fdt32_to_cpu(*address_cells_prop);
  *size_cells = fdt32_to_cpu(*size_cells_prop);
  return 0;
}

int get_reg(struct device *dev, const char *reg_name, struct reg *out_reg) {
  if (!dev->fdt) {
    dev_err(dev, "Device %s has no device tree node", dev->name);
    return -ENODEV;
  }
  int address_cells, size_cells;
  if (get_parent_size_rules(dev, &address_cells, &size_cells) < 0) {
    dev_err(dev, "Failed to get parent size rules for device %s", dev->name);
    return -EINVAL;
  }

  if (address_cells != 1 || (size_cells != 1 && size_cells != 0)) {
    dev_err(dev, "Unsupported address/size cells for device %s", dev->name);
    return -EINVAL;
  }

  if (reg_name == NULL) {
    // If reg_name is NULL, use the first register (index 0)
    int len;
    const fdt32_t *reg_prop =
        fdt_getprop(dev->fdt, dev->fdt_node_offset, "reg", &len);
    if (!reg_prop || len < 1 * sizeof(fdt32_t)) {
      dev_err(dev, "Failed to get reg property for device %s", dev->name);
      return -EINVAL;
    }
    if (address_cells + size_cells > len / sizeof(fdt32_t)) {
      dev_err(dev, "Not enough cells in reg property for device %s", dev->name);
      return -EINVAL;
    }

    out_reg->base = fdt32_to_cpu(reg_prop[0]);
    if (size_cells == 0) {
      out_reg->size = 0;
    } else {
      out_reg->size = fdt32_to_cpu(reg_prop[1]);
    }
    return 0;
  }
  int len = fdt_stringlist_count(dev->fdt, dev->fdt_node_offset, "reg-names");
  if (len < 0) {
    dev_err(dev, "Failed to get reg-names property for device %s", dev->name);
    return -EINVAL;
  }
  int reg_index = -1;
  for (int i = 0; i < len; i++) {
    const char *name = fdt_stringlist_get(dev->fdt, dev->fdt_node_offset,
                                          "reg-names", i, NULL);
    if (name && strcmp(name, reg_name) == 0) {
      reg_index = i;
      break;
    }
  }
  if (reg_index < 0) {
    dev_err(dev, "Failed to find reg-names entry %s for device %s", reg_name,
            dev->name);
    return -EINVAL;
  }
  const fdt32_t *reg_prop =
      fdt_getprop(dev->fdt, dev->fdt_node_offset, "reg", &len);
  if (!reg_prop ||
      len < (reg_index + 1) * (address_cells + size_cells) * sizeof(fdt32_t)) {
    dev_err(dev, "Failed to get reg property for device %s", dev->name);
    return -EINVAL;
  }
  out_reg->base =
      fdt32_to_cpu(reg_prop[reg_index * (address_cells + size_cells)]);
  if (size_cells == 0) {
    out_reg->size = 0;
  } else {
    out_reg->size =
        fdt32_to_cpu(reg_prop[reg_index * (address_cells + size_cells) + 1]);
  }
  return 0;
}

int get_prop_int32(struct device *dev, const char *prop_name, int *out_value) {
  if (!dev->fdt) {
    dev_err(dev, "Device %s has no device tree node", dev->name);
    return -ENODEV;
  }

  int len;
  const fdt32_t *prop =
      fdt_getprop(dev->fdt, dev->fdt_node_offset, prop_name, &len);

  if (!prop) {
    dev_dbg(dev, "Property %s for device %s not found", prop_name, dev->name);
    return -ENOENT;
  }
  if (len < sizeof(fdt32_t)) {
    dev_err(dev, "Property %s for device %s is too short", prop_name,
            dev->name);
    return -EINVAL;
  }
  *out_value = fdt32_to_cpu(*prop);
  return 0;
}

int get_prop_int32_default(struct device *dev, const char *prop_name,
                           int *out_value, int default_value) {
  int ret = get_prop_int32(dev, prop_name, out_value);
  if (ret == -ENOENT) {
    dev_dbg(dev, "Property %s for device %s not found, using default value",
            prop_name, dev->name);
    *out_value = default_value;
    ret = 0; // Return 0 to indicate that we used the default value
  }
  return ret;
}

int get_prop_bool(struct device *dev, const char *prop_name, bool *out_value) {
  int value;
  int ret = get_prop_int32(dev, prop_name, &value);
  if (ret < 0) {
    return ret;
  }
  *out_value = (value != 0);
  return 0;
}

int get_prop_bool_default(struct device *dev, const char *prop_name,
                          bool *out_value, bool default_value) {
  int ret = get_prop_bool(dev, prop_name, out_value);
  if (ret == -ENOENT) {
    dev_dbg(dev, "Property %s for device %s not found, using default value",
            prop_name, dev->name);
    *out_value = default_value;
    ret = 0; // Return 0 to indicate that we used the default value
  }
  return ret;
}

unsigned int get_prop_uint32(struct device *dev, const char *prop_name,
                             unsigned int *out_value) {
  if (!dev->fdt) {
    dev_err(dev, "Device %s has no device tree node", dev->name);
    return -ENODEV;
  }

  int len;
  const fdt32_t *prop =
      fdt_getprop(dev->fdt, dev->fdt_node_offset, prop_name, &len);

  if (!prop) {
    dev_dbg(dev, "Property %s for device %s not found", prop_name, dev->name);
    return -ENOENT;
  }
  if (len < sizeof(fdt32_t)) {
    dev_err(dev, "Property %s for device %s is too short", prop_name,
            dev->name);
    return -EINVAL;
  }
  *out_value = fdt32_to_cpu(*prop);
  return 0;
}

unsigned int get_prop_uint32_default(struct device *dev, const char *prop_name,
                                     unsigned int *out_value,
                                     unsigned int default_value) {
  int ret = get_prop_uint32(dev, prop_name, out_value);
  if (ret == -ENOENT) {
    dev_dbg(dev, "Property %s for device %s not found, using default value",
            prop_name, dev->name);
    *out_value = default_value;
    ret = 0; // Return 0 to indicate that we used the default value
  }
  return ret;
}

bool has_prop(struct device *dev, const char *prop_name) {
  if (!dev->fdt) {
    dev_err(dev, "Device %s has no device tree node", dev->name);
    return false;
  }

  const fdt32_t *prop =
      fdt_getprop(dev->fdt, dev->fdt_node_offset, prop_name, NULL);

  return prop != NULL;
}
