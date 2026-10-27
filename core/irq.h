#pragma once

#include "list.h"
#include <stddef.h>
#include <stdint.h>

struct device;

typedef enum {
  IRQ_NONE = 0,
  IRQ_HANDLED = 1,
} irqreturn_t;

typedef enum irq_event_type {
  IRQ_EVENT_NONE = 0,
  IRQ_EVENT_RISING_EDGE = 1,
  IRQ_EVENT_FALLING_EDGE = 1 << 1,
  IRQ_EVENT_BOTH_EDGES = IRQ_EVENT_RISING_EDGE | IRQ_EVENT_FALLING_EDGE,
  IRQ_EVENT_HIGH_LEVEL = 1 << 2,
  IRQ_EVENT_LOW_LEVEL = 1 << 3,
} irq_event_t;

struct irq_node {
  int irq;
  irqreturn_t (*handler)(int irq, irq_event_t, void *data);
  void *data;
  irq_event_t event;
  struct list_head list;
};

int devm_request_irq(struct device *dev, int irq,
                     irqreturn_t (*handler)(int, irq_event_t, void *data),
                     void *data);
void devm_free_irq(struct device *dev, int irq);

struct irq_data {
  int irq;
  int hw_irq;
  irq_event_t event;
  void *chip_data;
  struct irq_domain *domain;
  struct list_head list;
};

struct irq_domain {
  struct list_head list;
  struct device *master;
  struct irq_domain_ops *ops;
  struct irq_chip *chip;
  struct list_head map_list;
  int start, size;
};

struct irq_domain_ops {
  /**
   * @brief Map a hardware IRQ to a virtual IRQ in the domain.
   *
   * @param domain The IRQ domain.
   * @param data The IRQ data containing the hardware IRQ to map.
   * @return 0 on success, negative error code on failure.
   */
  int (*map)(struct irq_domain *domain, struct irq_data *data);

  /**
   * @brief Unmap a virtual IRQ from the domain.
   *
   * @param domain The IRQ domain.
   * @param data The IRQ data containing the virtual IRQ to unmap.
   */
  int (*unmap)(struct irq_domain *domain, struct irq_data *data);

  /**
   * @brief Translate device tree node and specifications to a hardware IRQ
   * number.
   *
   * @param domain The IRQ domain.
   * @param node_offset The device tree node offset.
   * @param intspec The arguments from the device tree.
   * @param intspec_len The number of arguments.
   * @param out_hw_irq Pointer to store the resulting hardware IRQ number.
   * @param out_event Pointer to store the resulting IRQ event type.
   * @return 0 on success, negative error code on failure.
   */
  int (*xlate)(struct irq_domain *domain, int node_offset,
               const uint32_t *intspec, int intspec_len, int *out_hw_irq,
               irq_event_t *out_event);
};

/**
 * @brief Create a new IRQ domain for a master device.
 *
 * @param master The master device that owns this IRQ domain.
 * @param start_irq The starting IRQ number for this domain.
 * @param num_irqs The number of IRQs in this domain.
 * @param ops The operations for mapping and unmapping IRQs in this domain. Can
 * be NULL if no special mapping is needed.
 * @return A pointer to the newly created irq_domain, or NULL on failure.
 */
struct irq_domain *irq_domain_create(struct device *master, int start_irq,
                                     int num_irqs, struct irq_domain_ops *ops);

int irq_domain_destroy(struct irq_domain *domain);

typedef int hw_irq_t;
int irq_domain_create_mapping(struct irq_domain *domain, hw_irq_t hw_irq);
struct irq_data *irq_domain_find_mapping(struct irq_domain *domain,
                                         hw_irq_t hw_irq);
struct irq_data *irq_domain_find_mapping_by_virq(int virq);
struct irq_domain *irq_domain_find_by_master(struct device *master);

struct irq_chip {
  const char *name;
  struct device *master;
  volatile int refcount;

  int (*init)(struct irq_chip *chip);
  int (*exit)(struct irq_chip *chip);
  int (*enable)(struct irq_chip *chip, struct irq_data *data);
  int (*disable)(struct irq_chip *chip, struct irq_data *data);
  int (*ack)(struct irq_chip *chip, struct irq_data *data);

  int (*set_type)(struct irq_chip *chip, struct irq_data *data,
                  irq_event_t event);
  int (*free)(struct irq_chip *chip, struct irq_data *data);

  // masking: optional
  int (*mask)(struct irq_chip *chip, struct irq_data *data);
  int (*unmask)(struct irq_chip *chip, struct irq_data *data);
};

int generic_irq_handle(struct irq_domain *domain, int virq, irq_event_t event);