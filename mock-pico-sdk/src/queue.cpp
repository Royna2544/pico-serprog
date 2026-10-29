#include "pico/util/queue.h"

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
struct QueueState {
  QueueState(size_t element_size, size_t capacity)
      : storage(element_size * capacity), element_size(element_size),
        capacity(capacity) {}

  std::mutex mutex;
  std::condition_variable not_empty;
  std::condition_variable not_full;
  std::vector<uint8_t> storage;
  const size_t element_size;
  const size_t capacity;
  size_t head = 0;
  size_t count = 0;
};

QueueState *state(queue_t *q) {
  return q ? static_cast<QueueState *>(q->impl) : nullptr;
}

bool add(queue_t *q, const void *data, bool block) {
  QueueState *s = state(q);
  if (!s) return false;
  std::unique_lock<std::mutex> lock(s->mutex);
  if (block) {
    s->not_full.wait(lock, [&] { return s->count < s->capacity; });
  } else if (s->count == s->capacity) {
    return false;
  }
  size_t tail = (s->head + s->count) % s->capacity;
  std::memcpy(s->storage.data() + tail * s->element_size, data, s->element_size);
  ++s->count;
  lock.unlock();
  s->not_empty.notify_one();
  return true;
}

bool remove(queue_t *q, void *data, bool block, bool peek) {
  QueueState *s = state(q);
  if (!s) return false;
  std::unique_lock<std::mutex> lock(s->mutex);
  if (block) {
    s->not_empty.wait(lock, [&] { return s->count != 0; });
  } else if (s->count == 0) {
    return false;
  }
  if (data)
    std::memcpy(data, s->storage.data() + s->head * s->element_size,
                s->element_size);
  if (!peek) {
    s->head = (s->head + 1) % s->capacity;
    --s->count;
    lock.unlock();
    s->not_full.notify_one();
  }
  return true;
}
} // namespace

extern "C" {
bool queue_init(queue_t *q, int element_size, int element_count) {
  if (!q || element_size <= 0 || element_count < 0 ||
      static_cast<size_t>(element_count) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(element_size))
    return false;
  // As with the SDK, reinitializing an initialized queue requires queue_free.
  q->impl = nullptr;
  try {
    q->impl = new QueueState(static_cast<size_t>(element_size),
                             static_cast<size_t>(element_count));
  } catch (const std::bad_alloc &) {
    return false;
  } catch (const std::length_error &) {
    return false;
  }
  q->element_size = element_size;
  q->element_count = element_count;
  return true;
}

void queue_free(queue_t *q) {
  if (!q) return;
  delete state(q);
  q->impl = nullptr;
}

bool queue_is_empty(queue_t *q) {
  QueueState *s = state(q);
  if (!s) return true;
  std::lock_guard<std::mutex> lock(s->mutex);
  return s->count == 0;
}

bool queue_is_full(queue_t *q) {
  QueueState *s = state(q);
  if (!s) return false;
  std::lock_guard<std::mutex> lock(s->mutex);
  return s->count == s->capacity;
}

bool queue_try_add(queue_t *q, const void *data) { return add(q, data, false); }
bool queue_try_remove(queue_t *q, void *data) {
  return remove(q, data, false, false);
}
bool queue_try_peek(queue_t *q, void *data) {
  return remove(q, data, false, true);
}

void queue_add_blocking(queue_t *q, const void *data) {
  if (!add(q, data, true)) std::abort(); // SDK requires a successfully initialized queue
}
void queue_remove_blocking(queue_t *q, void *data) {
  if (!remove(q, data, true, false)) std::abort();
}
void queue_peek_blocking(queue_t *q, void *data) {
  if (!remove(q, data, true, true)) std::abort();
}
}
