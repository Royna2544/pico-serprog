#include "irq.h"
#include "core/driver.h"
#include "list.h"
#include <errno.h>
#include <stdlib.h>

static DEFINE_LIST_HEAD(irq_node_list);
static DEFINE_LIST_HEAD(irq_domain_list);

int devm_request_irq(struct device *dev, int irq,
                     irqreturn_t (*handler)(int, irq_event_t, void *data),
                     void *drvdata) {
  int rc;
  // Check if the IRQ is already registered
  struct list_head *pos;
  list_for_each(pos, &irq_node_list) {
    struct irq_node *existing_node = container_of(pos, struct irq_node, list);
    if (existing_node->irq == irq) {
      return -EEXIST; // IRQ already registered
    }
  }

  struct irq_data *irq_data = irq_domain_find_mapping_by_virq(irq);
  if (IS_ERR_OR_NULL(irq_data)) {
    dev_err(dev, "IRQ: No IRQ mapping found for IRQ %d", irq);
    return -EINVAL;
  }
  struct irq_domain *domain = irq_data->domain;
  if (!domain || !domain->chip) {
    dev_err(dev, "IRQ: No IRQ domain or chip found for IRQ %d", irq);
    return -EINVAL;
  }
  struct irq_node *n = calloc(1, sizeof(struct irq_node));
  if (!n) {
    return -ENOMEM;
  }

  dev_dbg(dev, "Requesting IRQ %d for device %s, refcount %d", irq, dev->name,
          domain->chip->refcount);
  if (domain->chip->refcount == 0) {
    dev_dbg(domain->chip->master, "Initializing IRQ chip");
    rc = domain->chip->init(domain->chip);
    if (rc) {
      dev_err(domain->chip->master, "IRQ: Failed to initialize IRQ chip");
      goto free_node;
    }
  }

  domain->chip->refcount++;
  rc = domain->chip->set_type(domain->chip, irq_data, irq_data->event);
  if (rc) {
    dev_err(domain->chip->master, "IRQ: Failed to set type for IRQ %d", irq);
    goto irqchip_deinit;
  }
  n->irq = irq;
  n->handler = handler;
  n->data = drvdata;
  n->event = irq_data->event; // Store the event type for the handler;
  list_add_tail(&n->list, &irq_node_list);

  rc = domain->chip->enable(domain->chip, irq_data);
  if (rc) {
    dev_err(domain->chip->master, "IRQ: Failed to enable IRQ %d", irq);
    goto cleanup;
  }
  return 0;

cleanup:
  list_del(&n->list);
irqchip_deinit:
  domain->chip->refcount--;
  if (domain->chip->refcount == 0) {
    domain->chip->exit(domain->chip);
  }
free_node:
  free(n);
  return rc;
}

void devm_free_irq(struct device *dev, int irq) {
  struct list_head *pos, *tmp;
  list_for_each_safe(pos, tmp, &irq_node_list) {
    struct irq_node *node = container_of(pos, struct irq_node, list);
    if (node->irq == irq) {
      // Stop delivery before unpublishing the handler.
      struct irq_data *data = irq_domain_find_mapping_by_virq(irq);
      struct irq_domain *domain = IS_ERR_OR_NULL(data) ? NULL : data->domain;
      if (!IS_ERR_OR_NULL(domain) && !IS_ERR_OR_NULL(domain->chip)) {
        domain->chip->disable(domain->chip, data);
        list_del(&node->list);
        free(node);
        dev_dbg(dev, "Freeing IRQ %d for device %s, refcount %d", irq,
                dev->name, domain->chip->refcount);
        if (domain->chip->refcount == 1) {
          domain->chip->exit(domain->chip);
        }
        domain->chip->refcount--;
      } else {
        list_del(&node->list);
        free(node);
      }
      break;
    }
  }
}

static int gIrqNum = 40;
int irq_domain_create_mapping(struct irq_domain *domain, hw_irq_t offset) {
  if (!domain) {
    return -EINVAL;
  }
  if (offset < 0 || offset >= domain->size) {
    dev_err(domain->master, "IRQ offset %d exceeds domain size %d", offset,
            domain->size);
    return -EINVAL;
  }
  struct irq_data *irq_data = irq_domain_find_mapping(domain, offset);
  if (!IS_ERR(irq_data)) {
    dev_err(domain->master, "IRQ offset %d is already mapped in domain",
            offset);
    return -EEXIST;
  }
  struct irq_data *data = calloc(1, sizeof(struct irq_data));
  if (!data) {
    return -ENOMEM;
  }
  data->hw_irq = offset;
  data->domain = domain;

  if (offset >= domain->size) {
    dev_err(domain->master, "IRQ offset %d exceeds domain size %d", offset,
            domain->size);
    free(data);
    return -EINVAL;
  }
  data->irq = gIrqNum++; // Assign a virtual IRQ number

  dev_dbg(domain->master, "Hardware IRQ %d to virtual IRQ %d in domain", offset,
          data->irq);

  list_add_tail(&data->list, &domain->map_list);
  return data->irq; // Return the virtual IRQ number
}

int irq_domain_destroy(struct irq_domain *domain) {
  if (!domain) {
    return -EINVAL;
  }

  struct list_head *pos, *tmp;
  list_for_each_safe(pos, tmp, &domain->map_list) {
    struct irq_data *data = container_of(pos, struct irq_data, list);
    if (domain->ops && domain->ops->unmap) {
      domain->ops->unmap(domain, data);
    }
    list_del(&data->list);
    free(data);
  }
  list_del(&domain->list);
  free(domain);

  return 0;
}

struct irq_data *irq_domain_find_mapping(struct irq_domain *domain,
                                         hw_irq_t hw_irq) {
  if (!domain) {
    return ERR_PTR(-EINVAL);
  }

  struct list_head *pos;
  list_for_each(pos, &domain->map_list) {
    struct irq_data *data = container_of(pos, struct irq_data, list);
    if (data->hw_irq == hw_irq) {
      return data; // Return the virtual IRQ number
    }
  }

  return ERR_PTR(-ENOENT); // Mapping not found
}

struct irq_data *irq_domain_find_mapping_by_virq(int virq) {
  struct list_head *pos;
  list_for_each(pos, &irq_domain_list) {
    struct irq_domain *domain = container_of(pos, struct irq_domain, list);
    struct list_head *map_pos;
    list_for_each(map_pos, &domain->map_list) {
      struct irq_data *data = container_of(map_pos, struct irq_data, list);
      if (data->irq == virq) {
        return data; // Found the mapping for the given virtual IRQ
      }
    }
  }
  return ERR_PTR(-ENOENT); // No domain found for the given virtual IRQ
}

struct irq_domain *irq_domain_create(struct device *master, int start_irq,
                                     int num_irqs, struct irq_domain_ops *ops) {
  struct irq_domain *domain = calloc(1, sizeof(struct irq_domain));
  if (!domain) {
    return NULL;
  }
  domain->master = master;
  domain->ops = ops;
  domain->start = start_irq;
  domain->size = num_irqs;
  INIT_LIST_HEAD(&domain->map_list);
  list_add_tail(&domain->list, &irq_domain_list);
  return domain;
}

struct irq_domain *irq_domain_find_by_master(struct device *master) {
  if (!master) {
    return ERR_PTR(-EINVAL);
  }
  struct list_head *pos;
  list_for_each(pos, &irq_domain_list) {
    struct irq_domain *domain = container_of(pos, struct irq_domain, list);
    if (domain->master == master) {
      return domain;
    }
  }
  return ERR_PTR(-ENOENT); // No domain found for the given master device
}

int generic_irq_handle(struct irq_domain *domain, int virq, irq_event_t event) {
  if (!domain) {
    return -EINVAL;
  }
  struct list_head *pos;
  list_for_each(pos, &domain->map_list) {
    struct irq_data *data = container_of(pos, struct irq_data, list);
    if (data->irq == virq) {
      struct irq_chip *chip = domain->chip;
      struct list_head *node_pos;
      list_for_each(node_pos, &irq_node_list) {
        struct irq_node *node = container_of(node_pos, struct irq_node, list);
        if (node->irq == virq && node->handler) {
          switch (node->handler(node->irq, event, node->data)) {
          case IRQ_HANDLED:
            dev_dbg(domain->master, "IRQ %d handled successfully", virq);
            return 0;
          case IRQ_NONE:
            // The driver was unable to handle, disable the IRQ to prevent
            // further spurious interrupts. Especially important for
            // level-triggered interrupts, as the line may remain asserted.
            // Since the driver cannot handle it,
            // we assume it's a hardware issue or misconfiguration.
            if (chip && chip->disable &&
                (event == IRQ_EVENT_HIGH_LEVEL ||
                 event == IRQ_EVENT_LOW_LEVEL)) {
              dev_err(domain->master, "IRQ %d was not handled, disabling it",
                      virq);
              chip->disable(chip, data);
            }
            return -ENOENT;
          default:
            dev_err(domain->master, "IRQ %d handler returned unknown value",
                    virq);
            return -EINVAL;
          }
        }
      }
    }
  }
  return -ENOENT;
}
