#pragma once

#include "driver.h"
#include <stdint.h>

struct gpio_desc;

extern const char *hardware_model;
bool has_property(int node_offset, const char *property_name);

int apply_pinctrl_default(const struct device *dev);
int remove_pinctrl_default(const struct device *dev);

// Request a GPIO pin from the GPIO controller associated with the given device.
// With given prop name.
struct gpio_desc *gpio_request(struct device *dev, int node_offset,
                               const char *name);
int gpio_request_irq(struct device *dev, struct device *requester,
                     int node_offset, const char *name);
struct reg {
  uint32_t base;
  uint32_t size;
};

// Get the base address and size of a register from the device tree for a given
// device and node offset. The reg_name is used to find the index of the
// register in the "reg-names" property of the device tree node. If reg_name is
// NULL, the first register (index 0) will be used.
int get_reg(struct device *dev, const char *reg_name, struct reg *out_reg);

int get_prop_int32(struct device *dev, const char *prop_name, int *out_value);
int get_prop_int32_default(struct device *dev, const char *prop_name,
                           int *out_value, int default_value);
unsigned int get_prop_uint32(struct device *dev, const char *prop_name,
                             unsigned int *out_value);
unsigned int get_prop_uint32_default(struct device *dev, const char *prop_name,
                                     unsigned int *out_value,
                                     unsigned int default_value);
int get_prop_int32_array(struct device *dev, const char *prop_name,
                         int *out_values, int array_size);
bool has_prop(struct device *dev, const char *prop_name);

// Find a device by its phandle in the device tree, given a property name and
// index.
// By index, saying if the property is a list of integers, which one to get.
struct device *find_device_by_prop(struct device *dev, const char *prop_name,
                                   int fdt_offset, int index);