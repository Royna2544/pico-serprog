// Host regression for the real mock-pico-sdk queue implementation.
// Run with: bash tests/test-pico-queue.sh
#include "pico/util/queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__,         \
                   #condition);                                                 \
      std::abort();                                                             \
    }                                                                           \
  } while (0)

static void capacity() {
  queue_t q{};
  CHECK(queue_init(&q, sizeof(int), 2));
  int a = 11, b = 22, c = 33, out = -1;
  CHECK(queue_is_empty(&q));
  CHECK(!queue_is_full(&q));
  CHECK(queue_try_add(&q, &a));
  CHECK(!queue_is_full(&q));
  CHECK(queue_try_add(&q, &b));
  CHECK(queue_is_full(&q));
  CHECK(!queue_try_add(&q, &c));
  CHECK(queue_try_remove(&q, &out) && out == a);
  CHECK(queue_try_remove(&q, &out) && out == b);
  CHECK(!queue_try_remove(&q, &out));
  CHECK(queue_is_empty(&q));
  queue_free(&q);
}

static void try_add() {
  queue_t q{};
  CHECK(queue_init(&q, sizeof(int), 1));
  int source = 41, replacement = 99, out = -1;
  CHECK(queue_try_add(&q, &source));
  source = 77; // the enqueued value must be a copy, not a pointer
  CHECK(!queue_try_add(&q, &replacement));
  CHECK(queue_try_remove(&q, &out) && out == 41);
  CHECK(queue_is_empty(&q));
  CHECK(queue_try_add(&q, &replacement));
  CHECK(queue_try_remove(&q, &out) && out == replacement);
  queue_free(&q);
}

static void try_remove() {
  queue_t q{};
  CHECK(queue_init(&q, sizeof(int), 2));
  int a = 3, b = 4, out = 123;
  CHECK(!queue_try_remove(&q, &out) && out == 123);
  CHECK(queue_try_add(&q, &a));
  CHECK(queue_try_add(&q, &b));
  CHECK(queue_try_peek(&q, &out) && out == a);
  CHECK(queue_try_remove(&q, nullptr)); // NULL discards exactly one entry
  CHECK(queue_try_peek(&q, &out) && out == b);
  CHECK(queue_try_remove(&q, &out) && out == b);
  CHECK(!queue_try_peek(&q, &out));
  CHECK(!queue_try_remove(&q, &out) && out == b);
  queue_free(&q);
}

static void cross_thread() {
  using namespace std::chrono_literals;
  queue_t q{};
  CHECK(queue_init(&q, sizeof(int), 1));
  int first = 10, second = 20, out = -1;
  CHECK(queue_try_add(&q, &first));
  std::promise<void> started, finished;
  auto done = finished.get_future();
  std::thread writer([&] {
    started.set_value();
    queue_add_blocking(&q, &second);
    finished.set_value();
  });
  started.get_future().wait();
  bool returned_while_full = done.wait_for(80ms) == std::future_status::ready;
  CHECK(queue_try_remove(&q, &out) && out == first);
  bool woke_on_space = done.wait_for(2s) == std::future_status::ready;
  if (woke_on_space) writer.join();
  // If the worker is stuck, the test runner's watchdog terminates this test.
  CHECK(woke_on_space);
  CHECK(!returned_while_full);
  CHECK(queue_try_remove(&q, &out) && out == second);

  std::promise<void> reader_started, reader_finished;
  auto read_done = reader_finished.get_future();
  std::thread reader([&] {
    reader_started.set_value();
    queue_remove_blocking(&q, &out);
    reader_finished.set_value();
  });
  reader_started.get_future().wait();
  bool returned_while_empty = read_done.wait_for(80ms) == std::future_status::ready;
  CHECK(queue_try_add(&q, &first));
  bool woke_on_data = read_done.wait_for(2s) == std::future_status::ready;
  if (woke_on_data) reader.join();
  CHECK(woke_on_data);
  CHECK(!returned_while_empty);
  CHECK(out == first);
  queue_free(&q);
}

static void concurrent_fifo() {
  constexpr int producers = 2, consumers = 2, per_producer = 250;
  constexpr int total = producers * per_producer;
  queue_t q{};
  CHECK(queue_init(&q, sizeof(int), 7));
  std::vector<std::atomic<int>> seen(total);
  for (auto &entry : seen) entry.store(0);
  std::atomic<bool> invalid{false};
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      for (int i = 0; i < per_producer; ++i) {
        int id = p * per_producer + i;
        queue_add_blocking(&q, &id);
      }
    });
  }
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&] {
      for (int i = 0; i < total / consumers; ++i) {
        int id = -1;
        queue_remove_blocking(&q, &id);
        if (id < 0 || id >= total) invalid.store(true);
        else seen[id].fetch_add(1);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  CHECK(!invalid.load());
  for (auto &entry : seen) CHECK(entry.load() == 1);
  CHECK(queue_is_empty(&q));
  queue_free(&q);
}

static void concurrent_try() {
  constexpr int producers = 2, consumers = 2, per_producer = 250;
  constexpr int total = producers * per_producer;
  queue_t q{};
  CHECK(queue_init(&q, sizeof(int), 3));
  std::vector<std::atomic<int>> seen(total);
  for (auto &entry : seen) entry.store(0);
  std::atomic<bool> invalid{false};
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      for (int i = 0; i < per_producer; ++i) {
        int id = p * per_producer + i;
        while (!queue_try_add(&q, &id)) std::this_thread::yield();
      }
    });
  }
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&] {
      for (int i = 0; i < total / consumers; ++i) {
        int id = -1;
        while (!queue_try_remove(&q, &id)) std::this_thread::yield();
        if (id < 0 || id >= total) invalid.store(true);
        else seen[id].fetch_add(1);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  CHECK(!invalid.load());
  for (auto &entry : seen) CHECK(entry.load() == 1);
  CHECK(queue_is_empty(&q));
  queue_free(&q);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  const char *name = argv[1];
  if (std::string(name) == "capacity") capacity();
  else if (std::string(name) == "try_add") try_add();
  else if (std::string(name) == "try_remove") try_remove();
  else if (std::string(name) == "cross_thread") cross_thread();
  else if (std::string(name) == "concurrent_fifo") concurrent_fifo();
  else if (std::string(name) == "concurrent_try") concurrent_try();
  else return 2;
  std::printf("%s: PASS\n", name);
}
