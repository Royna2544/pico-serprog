#pragma once

#include <core/list.h>
#include <stdbool.h>
typedef struct stdio_driver stdio_driver_t;

struct stdio_driver {
  void (*out_chars)(stdio_driver_t *self, const char *buf,
                    int len); ///< Output characters to the driver; may be NULL
                              ///< if output is not supported
  void (*out_flush)(
      stdio_driver_t *self); ///< Flush any buffered output; may be NULL
  int (*in_chars)(
      stdio_driver_t *self, char *buf,
      int len); ///< Read available input characters; returns the number read,
                ///< or may be NULL if input is not supported
  void (*set_chars_available_callback)(
      stdio_driver_t *self, void (*fn)(void *),
      void *param); ///< Register a callback to be invoked when input characters
  ///< become available; may be NULL
  struct list_head list; ///< Internal list node for managing registered drivers
};

void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled);
static inline void stdio_filter_driver(stdio_driver_t *driver) {}
static inline void stdio_set_translate_crlf(stdio_driver_t *driver,
                                            bool translate) {}