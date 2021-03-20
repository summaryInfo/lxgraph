/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef LIST_H_
#define LIST_H_ 1

#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct list_head list_head_t;
typedef struct list_iter list_iter_t;

#define container_of(ptr, type, member) ({\
    const __typeof__(&((type *)0)->member) Mptr__ = ptr; \
    (type *)((char *)Mptr__ - offsetof(type, member)); })

struct list_head {
    list_head_t *prev;
    list_head_t *next;
};

struct list_iter {
    list_head_t *head;
    list_head_t *cur;
};

inline static void list_init(list_head_t *list) {
    list->next = list->prev = list;
}

inline static bool list_is_empty(list_head_t *list) {
    return list == list->next;
}

inline static list_head_t *list_erase(list_head_t *list) {
    list->prev->next = list->next;
    list->next->prev = list->prev;
    list->prev = list->next = NULL;
    return list;
}

inline static void list_append(list_head_t *restrict list, list_head_t *restrict elem) {
    elem->next = list->next;
    elem->prev = list;
    list->next->prev = elem;
    list->next = elem;
}

inline static list_iter_t list_begin(list_head_t *list) {
    return (list_iter_t) {
        .head = list,
        .cur = list->next,
    };
}

inline static list_head_t *list_current(list_iter_t *it) {
    return it->cur;
}

inline static list_head_t *list_next(list_iter_t *it) {
    if (it->cur == it->head) return NULL;
    list_head_t *cur = it->cur;
    it->cur = cur->next;
    return cur;
}

inline static list_head_t *list_erase_current(list_iter_t *it) {
    if (it->cur == it->head) return NULL;
    list_head_t *cur = it->cur;
    it->cur = cur->next;
    return list_erase(cur);
}

#endif
