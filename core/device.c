#include "core/irq.h"
#include "devicetree.h"
#include "driver.h"
#include "drivers/gpiolib.h"
#include "libfdt.h"
#include "list.h"
#include "printk.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>

static DEFINE_LIST_HEAD(devices);
static DEFINE_LIST_HEAD(drivers);

bool device_register(struct device *dev) {
  // Find matching subsys driver
  dev->deferred = false;
  dev->pinctrl_applied = false;
  int rc = 0;
  struct irq_data *new_irq_mapping = NULL;

  rc = apply_pinctrl_default(dev);
  int pinctrl_rc = rc;
  if (rc < 0 && rc != -ENOENT) {
    pr_err("DRVCORE: Failed to apply default pinctrl state for device %s",
           dev->name);
    return false;
  } else {
    dev->pinctrl_applied = true;
  }

  // Check interrupts-extended if the node wants irq.
  if (has_property(dev, dev->fdt_node_offset, "interrupts-extended")) {
    struct device *irq_dev = find_device_by_prop(dev, "interrupts-extended",
                                                 dev->fdt_node_offset, 0);
    if (!irq_dev) {
      pr_err("OF: Device %s has interrupts-extended property but no "
             "associated device found",
             dev->name);
      rc = EPROBE_DEFER;
      goto skip_probe;
    }

    // Find the IRQ domain associated with the interrupt device
    struct irq_domain *domain = irq_domain_find_by_master(irq_dev);
    if (IS_ERR_OR_NULL(domain)) {
      pr_err("OF: Device %s has interrupts-extended property but no "
             "associated irq_domain found for device %s",
             dev->name, irq_dev->name);
      rc = EPROBE_DEFER;
      goto skip_probe;
    }

    int len;
    const fdt32_t *intspec = fdt_getprop(dev->fdt, dev->fdt_node_offset,
                                         "interrupts-extended", &len);
    if (!intspec) {
      pr_err("OF: Device %s has interrupts-extended property but no "
             "data found",
             dev->name);
      rc = EPROBE_DEFER;
      goto skip_probe;
    }
    int hw_irq;
    irq_event_t event;

    if (!domain->ops || !domain->ops->xlate || !domain->ops->map) {
      pr_err("OF: IRQ domain for device %s has incomplete operations",
             dev->name);
      rc = -EINVAL;
      goto skip_probe;
    }

    // Get the interrupt number from the extended-interrupts property
    rc = domain->ops->xlate(domain, dev->fdt_node_offset, ++intspec,
                            (len / sizeof(fdt32_t)) - 1, &hw_irq, &event);
    if (rc < 0) {
      pr_err("OF: Failed to translate interrupts-extended for device %s",
             dev->name);
      goto skip_probe;
    }

    int ret = irq_domain_create_mapping(domain, hw_irq);
    if (ret < 0) {
      pr_err("OF: Failed to create IRQ mapping for device %s", dev->name);
      rc = ret;
      goto skip_probe;
    }

    struct irq_data *data = irq_domain_find_mapping(domain, hw_irq);
    if (IS_ERR(data)) {
      pr_err("OF: Failed to find IRQ mapping for device %s", dev->name);
      rc = PTR_ERR(data);
      goto skip_probe;
    }
    new_irq_mapping = data;

    rc = domain->ops->map(domain, data);
    if (rc < 0) {
      pr_err("OF: Failed to map IRQ for device %s", dev->name);
      goto skip_probe;
    }

    data->event = event;
  }

  // If the device has a parent, ensure that the parent has a driver bound
  // before probing this device
  if (dev->parent && !dev->parent->drv) {
    pr_err("DRVCORE: Device %s has parent '%s' with unbound driver", dev->name,
           dev->parent->name);
    // Defer probing this device until the parent is bound
    rc = EPROBE_DEFER;
  } else if (!dev->drv) {
    // The device-tree framework should have already assigned a driver to the
    // device based on its compatible string.
    pr_warn("DRVCORE: Device %s has no driver assigned", dev->name);
    rc = EPROBE_DEFER;
  } else {
    // Register the device in the global device list
    pr_info("DRVCORE: Registering device %s with driver %s, pdata %d",
            dev->name, dev->drv->name, dev->id);
    // Call the probe function of the driver to initialize the device
    rc = dev->drv->probe(dev);
  }

skip_probe:
  // Handle the result of the probe function
  if (rc == EPROBE_DEFER) {
    if (dev->drv)
      pr_warn("DRVCORE: Probe of device %s with driver %s deferred", dev->name,
              dev->drv->name);
    dev->deferred = true;
    goto reg;
  } else if (rc < 0) {
    pr_warn("DRVCORE: Probe of device %s with driver %s returned %d", dev->name,
            dev->drv->name, rc);
  }
  assert(rc <= 0);
  if (rc < 0) {
    if (new_irq_mapping) {
      struct irq_domain *domain = new_irq_mapping->domain;
      if (domain->ops->unmap)
        domain->ops->unmap(domain, new_irq_mapping);
      list_del(&new_irq_mapping->list);
      free(new_irq_mapping);
    }
    if (pinctrl_rc == 0) {
      remove_pinctrl_default(dev);
    }
    dev->drv = NULL;
    return false;
  }

reg:
  list_add(&dev->list, &devices);
  return true;
}

struct device *device_find_by_alias(const char *alias) {
  struct list_head *d;
  list_for_each(d, &devices) {
    struct device *dev = container_of(d, struct device, list);
    if (dev->alias && strcmp(dev->alias, alias) == 0) {
      return dev;
    }
  }
  return NULL;
}

bool device_unregister(struct device *dev) {
  struct list_head *d, *tmp;
  list_for_each_safe(d, tmp, &devices) {
    struct device *current_dev = container_of(d, struct device, list);
    if (current_dev == dev) {
      pr_info("DRVCORE: Unregistering device %s with driver %s", dev->name,
              dev->drv ? dev->drv->name : "(null)");
      if (!dev->deferred && dev->drv && dev->drv->remove) {
        dev->drv->remove(dev);
      }
      if (dev->pinctrl_applied) {
        remove_pinctrl_default(dev);
      }
      list_del(d);
      if (dev->free) {
        dev->free(dev);
      } else {
        free(dev);
      }
      return true;
    }
  }
  return false;
}

bool driver_register(struct driver *drv) {
  pr_info("DRVCORE: Registering driver %s", drv->name);
  list_add(&drv->list, &drivers);
  pr_info("DRVCORE: Registered driver '%s'", drv->name);
  return true;
}

static const struct driver *
driver_find_match(const char *compatible,
                  const struct driver_fdt_match_id **matched) {
  if (!compatible)
    return NULL;

  struct list_head *d;
  list_for_each(d, &drivers) {
    const struct driver *drv = container_of(d, struct driver, list);
    const struct driver_fdt_match_id *match = drv->match_table;
    while (match && match->compatible) {
      if (strcmp(match->compatible, compatible) == 0) {
        if (matched)
          *matched = match;
        return drv;
      }
      match++;
    }
  }
  return NULL;
}

const struct driver *driver_find_by_compatible(const char *compatible) {
  return driver_find_match(compatible, NULL);
}

const struct driver *driver_find_for_device(struct device *dev) {
  const struct driver_fdt_match_id *match = NULL;
  const struct driver *drv = NULL;
  dev->driver_data = NULL;
  int count = dev->fdt ? fdt_stringlist_count(dev->fdt, dev->fdt_node_offset,
                                              "compatible")
                       : 0;
  if (count > 0) {
    // Device-tree compatible strings are ordered from specific to general.
    for (int i = 0; i < count && !drv; i++) {
      const char *compatible = fdt_stringlist_get(
          dev->fdt, dev->fdt_node_offset, "compatible", i, NULL);
      drv = driver_find_match(compatible, &match);
    }
  } else {
    drv = driver_find_match(dev->compatible, &match);
  }
  if (match)
    dev->driver_data = match->data;
  return drv;
}

void drvcore_exit(void) {
  struct list_head *d, *tmp;
  while ((d = list_get_first(&devices))) {
    struct device *dev = container_of(d, struct device, list);
    if (!device_unregister(dev)) {
      pr_err("DRVCORE: Failed to unregister device %s", dev->name);
      break;
    }
  }
  list_for_each_safe(d, tmp, &drivers) {
    struct driver *drv = container_of(d, struct driver, list);
    pr_info("DRVCORE: Unregistering driver %s", drv->name);
    list_del(d);
  }
}

static int retry_deferred() {
  bool any_deferred = false;
  int first_error = 0;

  struct list_head *p;
  list_for_each(p, &devices) {
    struct device *dev = container_of(p, struct device, list);
    if (dev->deferred) {
      pr_info("DRVCORE: Retrying deferred probe for device %s id %d", dev->name,
              dev->id);
      any_deferred = true;
      if (dev->parent && !dev->parent->drv) {
        pr_warn("DRVCORE: Device %s has parent '%s' with unbound driver",
                dev->name, dev->parent->name);
        continue;
      }
      if (!dev->drv) {
        dev->drv = driver_find_for_device(dev);
        if (!dev->drv) {
          pr_warn(
              "DRVCORE: Still no driver found for device %s with compatible %s",
              dev->name, dev->compatible);
          continue;
        }
      }
      int rc = dev->drv->probe(dev);
      if (rc == 0) {
        pr_info("DRVCORE: Deferred probe succeeded");
        dev->deferred = false;
      } else if (rc == EPROBE_DEFER) {
        pr_warn("DRVCORE: Deferred probe still deferred");
      } else if (rc < 0) {
        pr_warn("DRVCORE: Deferred probe failed with code %d", rc);
        if (!first_error)
          first_error = rc;
      }
    }
  }

  if (!any_deferred) {
    pr_info("DRVCORE: No deferred devices to retry");
    return 0;
  }
  // Print a summary of deferred devices after retrying
  pr_info("DRVCORE: Summary of deferred devices after retry:");
  list_for_each(p, &devices) {
    struct device *dev = container_of(p, struct device, list);
    if (dev->deferred) {
      pr_info("DRVCORE: %s", dev->name);
    }
  }
  return first_error;
}
postcore_initcall(retry_deferred);

struct device *device_find_by_offset(const void *fdt, int node_offset) {
  struct list_head *p;
  list_for_each(p, &devices) {
    struct device *dev = container_of(p, struct device, list);
    if (dev->fdt == fdt && dev->fdt_node_offset == node_offset) {
      return dev;
    }
  }
  return NULL;
}

struct device *device_find_by_phandle(const void *fdt, int phandle) {
  int node_offset = fdt_node_offset_by_phandle(fdt, phandle);
  if (node_offset < 0) {
    pr_err("DRVCORE: Failed to find node by phandle %d", phandle);
    return NULL;
  }
  return device_find_by_offset(fdt, node_offset);
}
