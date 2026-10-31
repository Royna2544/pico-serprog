#include "drivers/flash_observer.h"

#include <string.h>

void flash_observer_begin(struct flash_observer *o) {
  if (!o)
    return;
  uint32_t next = o->session + 1;
  memset(o, 0, sizeof(*o));
  o->session = next ? next : 1;
  o->active = true;
}

static uint32_t address24(const uint8_t *tx) {
  return ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | tx[3];
}

static bool in_range(uint32_t addr, size_t len) {
  return addr < FLASH_BYTES && len && len <= FLASH_BYTES - addr;
}

/* Only call after validating the complete unit's bounds. */
static void mark(uint8_t *bits, uint32_t *count, uint32_t addr, uint32_t len,
                 uint32_t unit) {
  for (uint32_t i = addr / unit; i <= (addr + len - 1) / unit; ++i) {
    uint8_t mask = (uint8_t)(1u << (i & 7));
    if (!(bits[i >> 3] & mask)) {
      bits[i >> 3] |= mask;
      ++*count;
    }
  }
}

static bool result(struct flash_observer *o, struct flash_sample *out,
                   enum flash_kind kind, uint32_t addr, uint32_t len,
                   uint64_t now_us) {
  *out = (struct flash_sample){
      .session = o->session,
      .address = addr,
      .length = len,
      .erase4k = o->erase4k,
      .program256 = o->program256,
      .read256 = o->read256,
      .lost = o->lost,
      .kind = kind,
      .milestone =
          kind == FL_ID || kind == FL_UNSUP || kind == FL_CE ||
          ((kind != FL_BUSY && kind != FL_READY) && kind != o->last_kind),
  };
  if (kind != FL_BUSY && kind != FL_READY)
    o->last_kind = kind;
  o->last_activity_us = now_us;
  return true;
}

bool flash_observer_feed(struct flash_observer *o, const uint8_t *tx,
                         size_t slen, const uint8_t *rx, size_t rlen,
                         uint64_t now_us, struct flash_sample *out) {
  if (!o || !out || !tx || !slen || (rlen && !rx))
    return false;
  if (tx[0] == 0x9f) {
    if (slen != 1 || rlen < 3 || rlen > 16)
      return false;
    if (!o->active)
      flash_observer_begin(o);
    uint32_t id = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];
    o->supported = id == 0xc22017;
    return result(o, out, o->supported ? FL_ID : FL_UNSUP, id, 0, now_us);
  }
  if (!o->active || !o->supported)
    return false;
  if (tx[0] == 0x05) {
    if (slen != 1 || rlen != 1)
      return false;
    bool busy = (rx[0] & 1u) != 0;
    o->busy = busy;
    (void)result(o, out, busy ? FL_BUSY : FL_READY, 0, 0, now_us);
    out->milestone = false; /* WIP polls refresh state, not the phase FIFO. */
    return true;
  }
  if (rlen) {
    if (!((tx[0] == 0x03 && slen == 4) || (tx[0] == 0x0b && slen == 5)))
      return false;
    uint32_t addr = address24(tx);
    if (!in_range(addr, rlen))
      return false;
    mark(o->read, &o->read256, addr, (uint32_t)rlen, 256);
    return result(o, out, FL_RD, addr, (uint32_t)rlen, now_us);
  }
  if ((tx[0] == 0x60 || tx[0] == 0xc7) && slen == 1) {
    memset(o->erased, 0xff, sizeof(o->erased));
    o->erase4k = FLASH_BYTES / 4096;
    return result(o, out, FL_CE, 0, FLASH_BYTES, now_us);
  }
  if (tx[0] == 0x02) {
    if (slen < 4)
      return false;
    uint32_t addr = address24(tx);
    size_t len = slen - 4;
    if (len > 256 || !in_range(addr, len) || (addr & 255u) + len > 256)
      return false;
    mark(o->programmed, &o->program256, addr, (uint32_t)len, 256);
    return result(o, out, FL_PP, addr, (uint32_t)len, now_us);
  }
  if (slen != 4)
    return false;
  enum flash_kind kind;
  uint32_t size;
  switch (tx[0]) {
  case 0x20:
    kind = FL_SE;
    size = 4096;
    break;
  case 0x52:
    kind = FL_BE32;
    size = 32768;
    break;
  case 0xd8:
    kind = FL_BE64;
    size = 65536;
    break;
  default:
    return false;
  }
  uint32_t addr = address24(tx) & ~(size - 1);
  if (!in_range(addr, size))
    return false;
  mark(o->erased, &o->erase4k, addr, size, 4096);
  return result(o, out, kind, addr, size, now_us);
}
