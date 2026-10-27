#ifndef _DRIVER_H_
#define _DRIVER_H_

#include "list.h"
#include <stdbool.h>
#include <stddef.h>

struct device;

struct driver_fdt_match_id {
  const char *compatible;
  void *data;
};

struct driver {
  const char *name;
  struct list_head list;
  int (*probe)(struct device *device);
  void (*remove)(struct device *device);
  const struct driver_fdt_match_id *match_table;
};

struct device {
  const char *name;
  struct list_head list;
  int id;
  void *platform_data;
  const struct driver *drv;
  bool deferred, pinctrl_applied;
  int fdt_node_offset;
  const struct device *parent;
  const char *alias, *compatible;
  // Data associated with the driver, can be used in probe
  const void *driver_data, *fdt;

  // Optional function to customize free behavior for the device, otherwise,
  // free is called on the device struct itself
  void (*free)(struct device *dev);
};

bool device_register(struct device *dev);
bool device_unregister(struct device *dev);
struct device *device_find_by_alias(const char *alias);
struct device *device_find_by_offset(const void *fdt, int node_offset);
struct device *device_find_by_phandle(const void *fdt, int phandle);

__attribute__((format(printf, 2, 3))) void _dev_msg(struct device *dev,
                                                    const char *fmt, ...);

#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(ptr) ((intptr_t)(ptr))
#define IS_ERR(ptr) ((uintptr_t)(ptr) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(ptr) (!(ptr) || IS_ERR(ptr))

#include "printk.h"

#define __fallback_drv(dev)                                                    \
  (dev ? ((dev)->drv && (dev)->drv->name ? (dev)->drv->name                    \
                                         : "(NULL driver*)")                   \
       : "(NULL device*)")
#define __fallback_dev(dev)                                                    \
  ((dev) && (dev)->name ? (dev)->name : "(NULL device*)")
#define dev_info(dev, fmt, ...)                                                \
  pr_info("%s %s: " fmt, __fallback_drv(dev),                                  \
          __fallback_dev(dev), ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)                                                 \
  pr_err("%s %s: " fmt, __fallback_drv(dev), __fallback_dev(dev), ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)                                                \
  pr_warn("%s %s: " fmt, __fallback_drv(dev),                                  \
          __fallback_dev(dev), ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)                                                 \
  pr_debug("%s %s: " fmt, __fallback_drv(dev),                                 \
           __fallback_dev(dev), ##__VA_ARGS__)

bool driver_register(struct driver *drv);
const struct driver *driver_find_by_compatible(const char *compatible);
const struct driver *driver_find_for_device(struct device *dev);

#define container_of(ptr, type, member)                                        \
  ((type *)((char *)(ptr) - offsetof(type, member)))

#define EPROBE_DEFER -517

#include "init.h"
#define PLATFORM_DRIVER(plat)                                                  \
  static int plat##_register(void) { return !driver_register(&plat.drv); }     \
  core_initcall(plat##_register)

#endif
