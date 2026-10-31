#include "drivers/flash_telemetry.h"

bool flash_telemetry_init(struct flash_telemetry *t) {
  if (!t || !queue_init(&t->milestones, sizeof(struct flash_sample), 16))
    return false;
  if (!queue_init(&t->latest, sizeof(struct flash_sample), 1)) {
    queue_free(&t->milestones);
    return false;
  }
  return true;
}

void flash_telemetry_begin(struct flash_telemetry *t) {
  if (!t)
    return;
  flash_observer_begin(&t->observer);
  (void)queue_try_remove(&t->latest, NULL);
}

void flash_telemetry_feed(struct flash_telemetry *t, const uint8_t *tx,
                          size_t slen, const uint8_t *rx, size_t rlen,
                          uint64_t now_us) {
  struct flash_sample sample;
  if (!t ||
      !flash_observer_feed(&t->observer, tx, slen, rx, rlen, now_us, &sample))
    return;
  if (sample.milestone && !queue_try_add(&t->milestones, &sample)) {
    if (t->observer.lost != UINT32_MAX)
      ++t->observer.lost;
    sample.lost = t->observer.lost;
  }
  if (!queue_try_add(&t->latest, &sample)) {
    (void)queue_try_remove(&t->latest, NULL);
    (void)queue_try_add(&t->latest, &sample);
  }
}

bool flash_telemetry_milestone(struct flash_telemetry *t,
                               struct flash_sample *out) {
  return t && queue_try_remove(&t->milestones, out);
}

bool flash_telemetry_latest(struct flash_telemetry *t,
                            struct flash_sample *out) {
  return t && queue_try_remove(&t->latest, out);
}
