#include "pico/util/queue.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <queue>

#include <unordered_map>

static std::unordered_map<queue_t *, std::queue<std::vector<uint8_t>>> queues;

extern "C" {
/*! \brief Initialise a queue, allocating a (possibly shared) spinlock
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \param element_size Size of each value in the queue
 * \param element_count Maximum number of entries in the queue
 * \return true if the queue was initialized; false if it couldn't be (e.g.
 * malloc failed)
 */
bool queue_init(queue_t *q, int element_size, int element_count) {
  queues[q] = std::queue<std::vector<uint8_t>>();
  q->element_size = element_size;
  q->element_count = element_count;
  return true;
}

/*! \brief Destroy the specified queue.
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 *
 * Does not deallocate the queue_t structure itself.
 */
void queue_free(queue_t *q) { queues.erase(q); }

/*! \brief Check if queue is empty
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \return true if queue is empty, false otherwise
 *
 * This function is interrupt and multicore safe.
 */
bool queue_is_empty(queue_t *q) { return queues[q].empty(); }

/*! \brief Check if queue is full
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \return true if queue is full, false otherwise
 *
 * This function is interrupt and multicore safe.
 */
bool queue_is_full(queue_t *q) {
  return false; // For simplicity, we won't implement a maximum size for the
                // mock queue
}

// nonblocking queue access functions:

/*! \brief Non-blocking add value queue if not full
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \param data Pointer to value to be copied into the queue
 * \return true if the value was added
 *
 * If the queue is full this function will return immediately with false,
 * otherwise the data is copied into a new value added to the queue, and this
 * function will return true.
 */
bool queue_try_add(queue_t *q, const void *data) {
  if (queue_is_full(q))
    return false;
  std::vector<uint8_t> value(reinterpret_cast<const uint8_t *>(data),
                             reinterpret_cast<const uint8_t *>(data) +
                                 q->element_size);
  queues[q].push(value);
  return true;
}

/*! \brief Non-blocking removal of entry from the queue if non empty
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \param data Pointer to the location to receive the removed value, or NULL if
 * the data isn't required
 * \return true if a value was removed
 *
 * If the queue is not empty function will copy the removed value into the
 * location provided and return immediately with true, otherwise the function
 * will return immediately with false.
 */
bool queue_try_remove(queue_t *q, void *data) {
  if (queue_is_empty(q))
    return false;
  if (data) {
    std::vector<uint8_t> value = queues[q].front();
    std::memcpy(data, value.data(), value.size());
    printf("queue_try_remove: removed value of size %zu\n", value.size());
  }
  queues[q].pop();
  return true;
}

/*! \brief Non-blocking peek at the next item to be removed from the queue
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \param data Pointer to the location to receive the peeked value, or NULL if
 * the data isn't required
 * \return true if there was a value to peek
 *
 * If the queue is not empty this function will return immediately with true
 * with the peeked entry copied into the location specified by the data
 * parameter, otherwise the function will return false.
 */
bool queue_try_peek(queue_t *q, void *data) {
  if (queue_is_empty(q))
    return false;
  if (data) {
    std::vector<uint8_t> value = queues[q].front();
    std::memcpy(data, value.data(), value.size());
  }
  return true;
}

// blocking queue access functions:

/*! \brief Blocking add of value to queue
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \param data Pointer to value to be copied into the queue
 *
 * If the queue is full this function will block, until a removal happens on the
 * queue
 *
 * \note Because this method can block, prefer \ref queue_try_add in an IRQ
 * context
 */
void queue_add_blocking(queue_t *q, const void *data) {
  while (!queue_try_add(q, data)) {
    // In a real implementation, you might want to yield or sleep here
  }
}

/*! \brief Blocking remove entry from queue
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \param data Pointer to the location to receive the removed value, or NULL if
 * the data isn't required
 *
 * If the queue is empty this function will block until a value is added.
 *
 * \note Because this method can block, prefer \ref queue_try_remove in an IRQ
 * context
 */
void queue_remove_blocking(queue_t *q, void *data) {
  while (!queue_try_remove(q, data)) {
    // In a real implementation, you might want to yield or sleep here
  }
}

/*! \brief Blocking peek at next value to be removed from queue
 *  \ingroup queue
 *
 * \param q Pointer to a queue_t structure, used as a handle
 * \param data Pointer to the location to receive the peeked value, or NULL if
 * the data isn't required
 *
 * If the queue is empty function will block until a value is added
 *
 * \note Because this method can block, prefer \ref queue_try_peek in an IRQ
 * context
 */
void queue_peek_blocking(queue_t *q, void *data) {
  while (!queue_try_peek(q, data)) {
    // In a real implementation, you might want to yield or sleep here
  }
}
}