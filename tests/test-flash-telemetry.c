#define _POSIX_C_SOURCE 200809L
#include "drivers/flash_telemetry.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

static void test_id_is_copied_to_both_queues(void) {
  struct flash_telemetry t = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};

  assert(flash_telemetry_init(&t));
  flash_telemetry_feed(&t, id, sizeof(id), mx, sizeof(mx), 1);
  assert(flash_telemetry_milestone(&t, &s));
  assert(s.kind == FL_ID && s.session == 1 && s.address == 0xc22017);
  assert(s.milestone && s.lost == 0);
  assert(!flash_telemetry_milestone(&t, &s));
  assert(flash_telemetry_latest(&t, &s));
  assert(s.kind == FL_ID && s.session == 1);
  assert(!flash_telemetry_latest(&t, &s));
  queue_free(&t.latest);
  queue_free(&t.milestones);
}

static void test_overflow_keeps_latest_and_counts_lost(void) {
  struct flash_telemetry t = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t pp[] = {0x02, 0, 0, 0, 0xa5};
  const uint8_t rd[] = {0x03, 0, 0, 0}, byte[] = {0xff};

  assert(flash_telemetry_init(&t));
  flash_telemetry_feed(&t, id, 1, mx, sizeof(mx), 1);
  for (int i = 0; i < 17; ++i) {
    if (i & 1)
      flash_telemetry_feed(&t, rd, sizeof(rd), byte, 1, i + 2);
    else
      flash_telemetry_feed(&t, pp, sizeof(pp), NULL, 0, i + 2);
  }
  assert(t.observer.lost == 2);
  assert(flash_telemetry_latest(&t, &s));
  assert(s.session == 1 && s.kind == FL_PP && s.length == 1);
  assert(s.program256 == 1 && s.read256 == 1 && s.lost == 2);
  assert(!flash_telemetry_latest(&t, &s));
  for (int i = 0; i < 16; ++i) {
    assert(flash_telemetry_milestone(&t, &s));
    assert(s.session == 1 && s.lost == 0 && s.milestone);
    assert(s.kind == (i == 0 ? FL_ID : ((i & 1) ? FL_PP : FL_RD)));
  }
  assert(!flash_telemetry_milestone(&t, &s));
  queue_free(&t.latest);
  queue_free(&t.milestones);
}

static void test_new_session_and_wip_snapshots(void) {
  struct flash_telemetry t = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t pp[] = {0x02, 0, 0, 0, 0xa5};
  const uint8_t poll[] = {0x05}, busy[] = {1}, ready[] = {0};

  assert(flash_telemetry_init(&t));
  flash_telemetry_feed(&t, id, 1, mx, sizeof(mx), 1);
  flash_telemetry_feed(&t, pp, sizeof(pp), NULL, 0, 2);
  assert(flash_telemetry_milestone(&t, &s) && s.kind == FL_ID);
  assert(flash_telemetry_milestone(&t, &s) && s.kind == FL_PP);
  flash_telemetry_begin(&t);
  assert(t.observer.session == 2 && !t.observer.supported);
  assert(!t.observer.program256 && !t.observer.lost);
  assert(!flash_telemetry_latest(&t, &s));
  flash_telemetry_feed(&t, pp, sizeof(pp), NULL, 0, 3);
  assert(!flash_telemetry_milestone(&t, &s));
  assert(!flash_telemetry_latest(&t, &s));
  flash_telemetry_feed(&t, id, 1, mx, sizeof(mx), 4);
  assert(flash_telemetry_milestone(&t, &s));
  assert(s.kind == FL_ID && s.session == 2 && s.program256 == 0);
  flash_telemetry_feed(&t, poll, 1, busy, 1, 5);
  assert(!flash_telemetry_milestone(&t, &s));
  assert(flash_telemetry_latest(&t, &s));
  assert(s.kind == FL_BUSY && !s.milestone && s.session == 2);
  flash_telemetry_feed(&t, poll, 1, busy, 1, 6);
  assert(!flash_telemetry_milestone(&t, &s));
  flash_telemetry_feed(&t, poll, 1, ready, 1, 7);
  assert(!flash_telemetry_milestone(&t, &s));
  assert(flash_telemetry_latest(&t, &s) && s.kind == FL_READY && !s.milestone);
  assert(!flash_telemetry_latest(&t, &s));
  queue_free(&t.latest);
  queue_free(&t.milestones);
}

static void test_invalid_input_does_not_displace_snapshot(void) {
  struct flash_telemetry t = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t pp[] = {0x02, 0, 0, 0, 0xa5};
  const uint8_t invalid[] = {0x21, 0, 0, 0};
  const uint8_t r0[] = {0x03, 0, 0, 0};
  const uint8_t r1[] = {0x03, 0, 1, 0};
  const uint8_t byte[] = {0xff};

  assert(!flash_telemetry_init(NULL));
  flash_telemetry_begin(NULL);
  flash_telemetry_feed(NULL, id, 1, mx, 3, 1);
  assert(!flash_telemetry_milestone(NULL, &s));
  assert(!flash_telemetry_latest(NULL, &s));
  assert(flash_telemetry_init(&t));
  flash_telemetry_feed(&t, id, 1, mx, 3, 1);
  flash_telemetry_feed(&t, invalid, sizeof(invalid), NULL, 0, 2);
  flash_telemetry_feed(&t, pp, sizeof(pp), NULL, 1, 3);
  flash_telemetry_feed(&t, id, 1, mx, 2, 4);
  assert(flash_telemetry_latest(&t, &s) && s.kind == FL_ID);
  assert(!flash_telemetry_latest(&t, &s));
  assert(flash_telemetry_milestone(&t, &s) && s.kind == FL_ID);
  assert(!flash_telemetry_milestone(&t, &s));

  flash_telemetry_feed(&t, r0, sizeof(r0), byte, 1, 5);
  flash_telemetry_feed(&t, r1, sizeof(r1), byte, 1, 6);
  assert(flash_telemetry_milestone(&t, &s));
  assert(s.kind == FL_RD && s.read256 == 1 && s.milestone);
  assert(!flash_telemetry_milestone(&t, &s));
  assert(flash_telemetry_latest(&t, &s));
  assert(s.kind == FL_RD && s.read256 == 2 && !s.milestone);
  assert(!flash_telemetry_latest(&t, &s));
  assert(!t.observer.lost);
  queue_free(&t.latest);
  queue_free(&t.milestones);
}

static void test_lost_counter_saturates(void) {
  struct flash_telemetry t = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t pp[] = {0x02, 0, 0, 0, 0xa5};
  const uint8_t rd[] = {0x03, 0, 0, 0}, byte[] = {0xff};

  assert(flash_telemetry_init(&t));
  flash_telemetry_feed(&t, id, 1, mx, sizeof(mx), 1);
  for (int i = 0; i < 15; ++i) {
    if (i & 1)
      flash_telemetry_feed(&t, rd, sizeof(rd), byte, 1, i + 2);
    else
      flash_telemetry_feed(&t, pp, sizeof(pp), NULL, 0, i + 2);
  }
  t.observer.lost = UINT32_MAX;
  flash_telemetry_feed(&t, rd, sizeof(rd), byte, 1, 20);
  assert(t.observer.lost == UINT32_MAX);
  assert(flash_telemetry_latest(&t, &s) && s.lost == UINT32_MAX);
  queue_free(&t.latest);
  queue_free(&t.milestones);
}

static void test_wip_churn_does_not_evict_phase_milestones(void) {
  struct flash_telemetry t = {0};
  struct flash_sample s;
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t pp[] = {0x02, 0, 0, 0, 0xa5};
  const uint8_t poll[] = {0x05}, busy[] = {1}, ready[] = {0};

  assert(flash_telemetry_init(&t));
  flash_telemetry_feed(&t, id, 1, mx, sizeof(mx), 1);
  for (int i = 0; i < 32; ++i) {
    flash_telemetry_feed(&t, pp, sizeof(pp), NULL, 0, 2 + 3 * i);
    flash_telemetry_feed(&t, poll, 1, busy, 1, 3 + 3 * i);
    flash_telemetry_feed(&t, poll, 1, ready, 1, 4 + 3 * i);
  }
  assert(t.observer.lost == 0 && "WIP polls displaced phase milestones");
  assert(flash_telemetry_milestone(&t, &s) && s.kind == FL_ID);
  assert(flash_telemetry_milestone(&t, &s) && s.kind == FL_PP);
  assert(!flash_telemetry_milestone(&t, &s));
  assert(flash_telemetry_latest(&t, &s) && s.kind == FL_READY);
  queue_free(&t.latest);
  queue_free(&t.milestones);
}

struct concurrent_consumer {
  struct flash_telemetry *telemetry;
  atomic_bool done;
  uint32_t program256, read256, milestones;
};

static void *drain_concurrently(void *arg) {
  struct concurrent_consumer *ctx = arg;
  struct flash_sample s;
  while (!atomic_load_explicit(&ctx->done, memory_order_acquire) ||
         !queue_is_empty(&ctx->telemetry->latest) ||
         !queue_is_empty(&ctx->telemetry->milestones)) {
    bool observed = false;
    if (flash_telemetry_latest(ctx->telemetry, &s)) {
      assert(s.session == 1 && s.erase4k == 0);
      assert(s.program256 >= ctx->program256 && s.read256 >= ctx->read256);
      ctx->program256 = s.program256;
      ctx->read256 = s.read256;
      observed = true;
    }
    if (flash_telemetry_milestone(ctx->telemetry, &s)) {
      assert(s.session == 1 && s.milestone);
      ++ctx->milestones;
      observed = true;
    }
    if (!observed)
      sched_yield();
  }
  return NULL;
}

static void test_concurrent_producer_and_consumer(void) {
  struct flash_telemetry t = {0};
  struct concurrent_consumer ctx = {.telemetry = &t};
  pthread_t thread;
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t byte[] = {0xa5};
  uint8_t pp[] = {0x02, 0, 0, 0, 0xa5};
  uint8_t rd[] = {0x03, 0, 0, 0};

  assert(flash_telemetry_init(&t));
  atomic_init(&ctx.done, false);
  flash_telemetry_feed(&t, id, 1, mx, sizeof(mx), 1);
  assert(!pthread_create(&thread, NULL, drain_concurrently, &ctx));
  for (uint32_t i = 0; i < 1024; ++i) {
    uint32_t addr = i * 256u;
    pp[1] = rd[1] = (uint8_t)(addr >> 16);
    pp[2] = rd[2] = (uint8_t)(addr >> 8);
    pp[3] = rd[3] = (uint8_t)addr;
    flash_telemetry_feed(&t, pp, sizeof(pp), NULL, 0, i + 2);
    flash_telemetry_feed(&t, rd, sizeof(rd), byte, 1, i + 2);
  }
  atomic_store_explicit(&ctx.done, true, memory_order_release);
  assert(!pthread_join(thread, NULL));
  assert(ctx.program256 == 1024 && ctx.read256 == 1024);
  assert(ctx.milestones + t.observer.lost == 1 + 2 * 1024);
  assert(queue_is_empty(&t.milestones) && queue_is_empty(&t.latest));
  queue_free(&t.latest);
  queue_free(&t.milestones);
}

int main(void) {
  test_id_is_copied_to_both_queues();
  test_overflow_keeps_latest_and_counts_lost();
  test_new_session_and_wip_snapshots();
  test_invalid_input_does_not_displace_snapshot();
  test_lost_counter_saturates();
  test_wip_churn_does_not_evict_phase_milestones();
  test_concurrent_producer_and_consumer();
  puts("PASS flash telemetry queue/coalescing/gap/session/WIP/concurrency");
  return 0;
}
