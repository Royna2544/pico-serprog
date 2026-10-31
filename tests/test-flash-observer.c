#include "drivers/flash_observer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_id_and_wren(void) {
  struct flash_observer o = {0};
  struct flash_sample s = {0};
  const uint8_t rdid[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t wren[] = {0x06};

  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, rdid, sizeof(rdid), mx, sizeof(mx), 1, &s));
  assert(s.kind == FL_ID && s.address == 0xc22017 && s.length == 0);
  assert(s.session == 1 && s.milestone && o.supported);
  assert(!flash_observer_feed(&o, wren, sizeof(wren), NULL, 0, 2, &s));
}

static void test_erase_coverage(void) {
  struct flash_observer o = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t other[] = {0xef, 0x40, 0x18};
  const uint8_t ce[] = {0xc7}, ce_alt[] = {0x60};
  const uint8_t se[] = {0x20, 0x00, 0x10, 0x03};
  const uint8_t be32[] = {0x52, 0x00, 0x8f, 0xff};
  const uint8_t be64[] = {0xd8, 0x01, 0xff, 0xff};
  const uint8_t beyond[] = {0x20, 0x80, 0x00, 0x00};
  const uint8_t wrong[] = {0x21, 0x00, 0x10, 0x00};

  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, id, 1, other, sizeof(other), 1, &s));
  assert(s.kind == FL_UNSUP && s.address == 0xef4018 && !o.supported);
  assert(!flash_observer_feed(&o, ce, 1, NULL, 0, 2, &s));
  assert(!o.erase4k);
  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, id, 1, mx, sizeof(mx), 3, &s));
  assert(s.session == 2 && !s.erase4k);
  assert(flash_observer_feed(&o, ce, 1, NULL, 0, 4, &s));
  assert(s.kind == FL_CE && s.length == FLASH_BYTES && s.erase4k == 2048);
  assert(s.milestone && o.erase4k == 2048);
  assert(flash_observer_feed(&o, ce_alt, 1, NULL, 0, 5, &s));
  assert(s.erase4k == 2048 && s.milestone);

  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, id, 1, mx, sizeof(mx), 6, &s));
  assert(flash_observer_feed(&o, se, sizeof(se), NULL, 0, 7, &s));
  assert(s.kind == FL_SE && s.address == 0x1000 && s.length == 4096);
  assert(s.erase4k == 1 && s.milestone);
  assert(flash_observer_feed(&o, se, sizeof(se), NULL, 0, 8, &s));
  assert(s.erase4k == 1 && !s.milestone);
  assert(flash_observer_feed(&o, be32, sizeof(be32), NULL, 0, 9, &s));
  assert(s.kind == FL_BE32 && s.address == 0x8000 && s.erase4k == 9);
  assert(flash_observer_feed(&o, be64, sizeof(be64), NULL, 0, 10, &s));
  assert(s.kind == FL_BE64 && s.address == 0x10000 && s.erase4k == 25);
  assert(!flash_observer_feed(&o, beyond, sizeof(beyond), NULL, 0, 11, &s));
  assert(!flash_observer_feed(&o, wrong, sizeof(wrong), NULL, 0, 12, &s));
  assert(!flash_observer_feed(&o, se, sizeof(se) - 1, NULL, 0, 13, &s));
  assert(!flash_observer_feed(&o, se, sizeof(se), mx, 3, 14, &s));
  assert(!flash_observer_feed(&o, ce, 0, NULL, 0, 15, &s));
  assert(o.erase4k == 25 && o.last_activity_us == 10);
}

static void test_program_page_coverage(void) {
  struct flash_observer o = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t pp[] = {0x02, 0x00, 0x01, 0x00, 0xaa, 0xbb};
  const uint8_t last[] = {0x02, 0x7f, 0xff, 0xff, 0x55};
  const uint8_t crossing[] = {0x02, 0x00, 0x01, 0xff, 0xaa, 0xbb};
  const uint8_t overflow[] = {0x02, 0x7f, 0xff, 0xff, 0xaa, 0xbb};
  const uint8_t beyond[] = {0x02, 0x80, 0x00, 0x00, 0xaa};
  const uint8_t empty[] = {0x02, 0x00, 0x00, 0x00};
  uint8_t too_long[4 + 257] = {0x02};

  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, id, 1, mx, sizeof(mx), 1, &s));
  assert(flash_observer_feed(&o, pp, sizeof(pp), NULL, 0, 2, &s));
  assert(s.kind == FL_PP && s.address == 0x100 && s.length == 2);
  assert(s.program256 == 1 && s.milestone);
  assert(flash_observer_feed(&o, pp, sizeof(pp), NULL, 0, 3, &s));
  assert(s.program256 == 1 && !s.milestone);
  assert(flash_observer_feed(&o, last, sizeof(last), NULL, 0, 4, &s));
  assert(s.program256 == 2);
  assert(!flash_observer_feed(&o, crossing, sizeof(crossing), NULL, 0, 5, &s));
  assert(!flash_observer_feed(&o, overflow, sizeof(overflow), NULL, 0, 6, &s));
  assert(!flash_observer_feed(&o, beyond, sizeof(beyond), NULL, 0, 7, &s));
  assert(!flash_observer_feed(&o, empty, sizeof(empty), NULL, 0, 8, &s));
  assert(!flash_observer_feed(&o, too_long, sizeof(too_long), NULL, 0, 9, &s));
  assert(!flash_observer_feed(&o, pp, sizeof(pp), mx, 3, 10, &s));
  assert(o.program256 == 2 && o.last_activity_us == 4);
}

static void test_read_page_coverage(void) {
  struct flash_observer o = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t rd[] = {0x03, 0x00, 0x01, 0x00};
  const uint8_t fast[] = {0x0b, 0x00, 0x02, 0x00, 0xff};
  const uint8_t split[] = {0x03, 0x00, 0x02, 0xff};
  const uint8_t last[] = {0x03, 0x7f, 0xff, 0xff};
  const uint8_t beyond[] = {0x03, 0x80, 0x00, 0x00};
  const uint8_t wrong[] = {0x13, 0x00, 0x01, 0x00};
  uint8_t data[256] = {0};
  uint8_t pair[2] = {0};

  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, id, 1, mx, sizeof(mx), 1, &s));
  assert(flash_observer_feed(&o, rd, sizeof(rd), data, sizeof(data), 2, &s));
  assert(s.kind == FL_RD && s.address == 0x100 && s.length == 256);
  assert(s.read256 == 1 && s.milestone);
  assert(
      flash_observer_feed(&o, fast, sizeof(fast), data, sizeof(data), 3, &s));
  assert(s.read256 == 2 && !s.milestone);
  assert(flash_observer_feed(&o, split, sizeof(split), pair, 2, 4, &s));
  assert(s.read256 == 3);
  assert(flash_observer_feed(&o, last, sizeof(last), pair, 1, 5, &s));
  assert(s.read256 == 4);
  assert(!flash_observer_feed(&o, last, sizeof(last), pair, 2, 6, &s));
  assert(!flash_observer_feed(&o, beyond, sizeof(beyond), data, 1, 7, &s));
  assert(!flash_observer_feed(&o, rd, sizeof(rd), data, 0, 8, &s));
  assert(!flash_observer_feed(&o, rd, sizeof(rd), NULL, 1, 9, &s));
  assert(!flash_observer_feed(&o, rd, 3, data, 1, 10, &s));
  assert(!flash_observer_feed(&o, fast, 4, data, 1, 11, &s));
  assert(!flash_observer_feed(&o, fast, 6, data, 1, 12, &s));
  assert(!flash_observer_feed(&o, wrong, sizeof(wrong), data, 1, 13, &s));
  assert(!flash_observer_feed(&o, rd, sizeof(rd), data, SIZE_MAX, 14, &s));
  assert(o.read256 == 4 && o.last_activity_us == 5);
}

static void test_wip_transitions(void) {
  struct flash_observer o = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t poll[] = {0x05}, busy[] = {0x81}, ready[] = {0x80};
  const uint8_t pp[] = {0x02, 0, 0, 0, 0xaa};

  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, id, 1, mx, sizeof(mx), 1, &s));
  assert(flash_observer_feed(&o, poll, 1, ready, 1, 2, &s));
  assert(s.kind == FL_READY && !s.milestone && !o.busy);
  assert(flash_observer_feed(&o, pp, sizeof(pp), NULL, 0, 3, &s));
  assert(flash_observer_feed(&o, poll, 1, busy, 1, 4, &s));
  assert(s.kind == FL_BUSY && !s.milestone && o.busy);
  for (uint64_t i = 5; i < 20; ++i) {
    assert(flash_observer_feed(&o, poll, 1, busy, 1, i, &s));
    assert(s.kind == FL_BUSY && !s.milestone && o.busy);
  }
  assert(o.last_activity_us == 19 && o.last_kind == FL_PP);
  assert(flash_observer_feed(&o, poll, 1, ready, 1, 20, &s));
  assert(s.kind == FL_READY && !s.milestone && !o.busy);
  assert(flash_observer_feed(&o, poll, 1, ready, 1, 21, &s));
  assert(!s.milestone);
  assert(!flash_observer_feed(&o, poll, 1, busy, 0, 22, &s));
  assert(!flash_observer_feed(&o, poll, 1, busy, 2, 23, &s));
  assert(!flash_observer_feed(&o, poll, 0, busy, 1, 24, &s));
  assert(!flash_observer_feed(&o, pp, sizeof(pp), NULL, 1, 25, &s));
  assert(o.last_activity_us == 21 && !o.busy);
}

static void test_malformed_id_and_null_arguments(void) {
  struct flash_observer o = {0};
  struct flash_sample s = {.session = 77, .kind = FL_IDLE};
  const uint8_t id[] = {0x9f}, malformed[] = {0x9f, 0x00};
  const uint8_t mx[] = {0xc2, 0x20, 0x17};
  uint8_t long_id[17] = {0xc2, 0x20, 0x17};
  const uint8_t se[] = {0x20, 0, 0, 0};

  assert(!flash_observer_feed(&o, se, sizeof(se), NULL, 0, 1, &s));
  assert(!flash_observer_feed(&o, id, 1, mx, 2, 2, &s));
  assert(!flash_observer_feed(&o, id, 1, long_id, sizeof(long_id), 3, &s));
  assert(!flash_observer_feed(&o, malformed, 2, mx, 3, 4, &s));
  assert(!flash_observer_feed(&o, id, 1, NULL, 3, 5, &s));
  assert(!flash_observer_feed(&o, NULL, 1, mx, 3, 6, &s));
  assert(!flash_observer_feed(&o, id, 0, mx, 3, 7, &s));
  assert(!flash_observer_feed(NULL, id, 1, mx, 3, 8, &s));
  assert(!flash_observer_feed(&o, id, 1, mx, 3, 9, NULL));
  assert(!o.active && !o.supported && !o.erase4k);
  assert(s.session == 77 && s.kind == FL_IDLE);
  assert(flash_observer_feed(&o, id, 1, mx, 3, 10, &s));
  assert(s.session == 1 && s.kind == FL_ID);
  assert(!flash_observer_feed(&o, malformed, 2, mx, 3, 11, &s));
  assert(o.supported && o.last_activity_us == 10);
  flash_observer_begin(&o);
  assert(o.session == 2 && !o.supported);
  assert(!flash_observer_feed(&o, se, sizeof(se), NULL, 0, 12, &s));
  o.session = UINT32_MAX;
  flash_observer_begin(&o);
  assert(o.session == 1);
  flash_observer_begin(NULL);
}

static void test_all_pages_count_once(void) {
  struct flash_observer o = {0};
  struct flash_sample s = {0};
  const uint8_t id[] = {0x9f}, mx[] = {0xc2, 0x20, 0x17};
  const uint8_t datum[] = {0xa5};
  uint8_t pp[] = {0x02, 0, 0, 0, 0xa5};
  uint8_t rd[] = {0x03, 0, 0, 0};

  flash_observer_begin(&o);
  assert(flash_observer_feed(&o, id, 1, mx, 3, 1, &s));
  for (uint32_t i = 0; i < FLASH_BYTES / 256u; ++i) {
    uint32_t addr = i * 256u;
    pp[1] = rd[1] = (uint8_t)(addr >> 16);
    pp[2] = rd[2] = (uint8_t)(addr >> 8);
    pp[3] = rd[3] = (uint8_t)addr;
    assert(flash_observer_feed(&o, pp, sizeof(pp), NULL, 0, 2, &s));
    assert(flash_observer_feed(&o, rd, sizeof(rd), datum, 1, 3, &s));
  }
  assert(o.program256 == FLASH_BYTES / 256u);
  assert(o.read256 == FLASH_BYTES / 256u);
  assert(s.program256 == FLASH_BYTES / 256u);
  assert(s.read256 == FLASH_BYTES / 256u);
  pp[1] = rd[1] = pp[2] = rd[2] = pp[3] = rd[3] = 0;
  assert(flash_observer_feed(&o, pp, sizeof(pp), NULL, 0, 4, &s));
  assert(flash_observer_feed(&o, rd, sizeof(rd), datum, 1, 5, &s));
  assert(s.program256 == FLASH_BYTES / 256u);
  assert(s.read256 == FLASH_BYTES / 256u);
  flash_observer_begin(&o);
  assert(o.program256 == 0 && o.read256 == 0);
  assert(!memcmp(o.programmed, (uint8_t[4096]){0}, sizeof(o.programmed)));
  assert(!memcmp(o.read, (uint8_t[4096]){0}, sizeof(o.read)));
}

int main(void) {
  test_id_and_wren();
  test_erase_coverage();
  test_program_page_coverage();
  test_read_page_coverage();
  test_wip_transitions();
  test_malformed_id_and_null_arguments();
  test_all_pages_count_once();
  puts("PASS flash observer ID/erase/program/read/WIP/bounds");
  return 0;
}
