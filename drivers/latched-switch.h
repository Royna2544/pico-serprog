#pragma once

#include "core/driver.h"

typedef void (*latched_switch_on_change_callback_t)(void *data, bool new_state);

struct latched_switch_driver {
  struct driver drv;
  int (*get_state)(struct device *dev, bool *state);
  int (*set_on_change_callback)(struct device *dev,
                                latched_switch_on_change_callback_t callback,
                                void *data);
};

int latched_switch_get_state(struct device *dev, bool *state);
int latched_switch_set_on_change_callback(
    struct device *dev, latched_switch_on_change_callback_t callback,
    void *data);