#ifndef FLASH_TELEMETRY_H
#define FLASH_TELEMETRY_H

#include "drivers/flash_observer.h"
#include <pico/util/queue.h>

/* Initialize once before the producer core starts. Thereafter only that core
 * calls begin/feed and the consumer core removes copied samples. The 16-slot
 * milestone FIFO drops on overflow (counted in lost), while the one-slot latest
 * snapshot replaces older progress without blocking the producer. */
struct flash_telemetry {
  struct flash_observer observer;
  queue_t milestones, latest;
};

bool flash_telemetry_init(struct flash_telemetry *t);
void flash_telemetry_begin(struct flash_telemetry *t);
void flash_telemetry_feed(struct flash_telemetry *t, const uint8_t *tx,
                          size_t slen, const uint8_t *rx, size_t rlen,
                          uint64_t now_us);
bool flash_telemetry_milestone(struct flash_telemetry *t,
                               struct flash_sample *out);
bool flash_telemetry_latest(struct flash_telemetry *t,
                            struct flash_sample *out);

#endif
