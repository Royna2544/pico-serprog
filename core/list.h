#pragma once

#include <stdbool.h>
#include <stdio.h>

struct list_head {
  struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) {&(name), &(name)}
#define DEFINE_LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
#define INIT_LIST_HEAD(ptr)                                                    \
  do {                                                                         \
    (ptr)->next = (ptr);                                                       \
    (ptr)->prev = (ptr);                                                       \
  } while (0)

static inline bool list_empty(const struct list_head *head) {
  return head->next == head;
}

static inline void __list_add(struct list_head *node, struct list_head *prev,
                              struct list_head *next) {
  node->prev = prev;
  node->next = next;
  prev->next = node;
  next->prev = node;
}

static inline void list_add(struct list_head *node, struct list_head *head) {
  __list_add(node, head, head->next);
}

static inline void list_add_tail(struct list_head *node,
                                 struct list_head *head) {
  __list_add(node, head->prev, head);
}

static inline void list_del(struct list_head *node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->next = node->prev = node;
}

static inline struct list_head *list_get_first(struct list_head *head) {
  if (list_empty(head)) {
    return NULL;
  } else {
    return head->next;
  }
}

#define list_for_each(pos, head)                                               \
  for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, tmp, head)                                     \
  for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head);             \
       (pos) = (tmp), (tmp) = (pos)->next)
