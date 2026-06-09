#ifndef KERNEL_LIST_H
#define KERNEL_LIST_H

#include <stddef.h>
#include <stdint.h>

struct list_node_t {
    list_node_t *prev;
    list_node_t *next;
};

static inline void list_init(list_node_t *head) {
    head->prev = head;
    head->next = head;
}

static inline void list_push_back(list_node_t *head, list_node_t *node) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

static inline void list_remove(list_node_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = node;
}

static inline bool list_empty(list_node_t *head) {
    return head->next == head;
}

static inline list_node_t *list_front(list_node_t *head) {
    return head->next;
}

#define LIST_ENTRY(node, type, member) \
    ((type *)((char *)(node) - offsetof(type, member)))

#endif // KERNEL_LIST_H