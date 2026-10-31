#ifndef FLASH_OBSERVER_H
#define FLASH_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLASH_BYTES (8u * 1024u * 1024u)

enum flash_kind {
  FL_ID,
  FL_UNSUP,
  FL_CE,
  FL_SE,
  FL_BE32,
  FL_BE64,
  FL_PP,
  FL_RD,
  FL_BUSY,
  FL_READY,
  FL_IDLE
};

struct flash_sample {
  uint32_t session, address, length, erase4k, program256, read256, lost;
  enum flash_kind kind;
  bool milestone;
};

/* Zero-initialize before the first begin/feed. Mutated only by the producer
 * core; consumers receive copied samples through flash_telemetry queues. */
struct flash_observer {
  uint8_t erased[256], programmed[4096], read[4096];
  uint32_t session, erase4k, program256, read256, lost;
  uint64_t last_activity_us;
  enum flash_kind last_kind;
  bool supported, busy, active;
};

void flash_observer_begin(struct flash_observer *o);
/* Call only after a successful full SPI transfer. Counts are unique addressed
 * units observed on the bus, never proof of a completed erase or program. */
bool flash_observer_feed(struct flash_observer *o, const uint8_t *tx,
                         size_t slen, const uint8_t *rx, size_t rlen,
                         uint64_t now_us, struct flash_sample *out);

#endif
