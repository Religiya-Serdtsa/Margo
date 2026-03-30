#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *data;
    size_t elem_size;
    size_t size;
    size_t capacity;
    size_t head;
} margo_ds_deque_t;

static inline void margo_ds_deque_init(margo_ds_deque_t *dq, size_t elem_size) {
    if (!dq) {
        return;
    }
    dq->data = NULL;
    dq->elem_size = elem_size;
    dq->size = 0;
    dq->capacity = 0;
    dq->head = 0;
}

static inline void margo_ds_deque_free(margo_ds_deque_t *dq) {
    if (!dq) {
        return;
    }
    free(dq->data);
    dq->data = NULL;
    dq->size = 0;
    dq->capacity = 0;
    dq->head = 0;
}

static inline bool margo_ds_deque_reserve(margo_ds_deque_t *dq, size_t new_capacity) {
    if (!dq || dq->elem_size == 0 || new_capacity <= dq->capacity) {
        return true;
    }
    void *next = malloc(new_capacity * dq->elem_size);
    if (!next) {
        return false;
    }
    for (size_t i = 0; i < dq->size; ++i) {
        size_t idx = (dq->head + i) % (dq->capacity ? dq->capacity : 1);
        memcpy((char *)next + i * dq->elem_size,
               (char *)dq->data + idx * dq->elem_size,
               dq->elem_size);
    }
    free(dq->data);
    dq->data = next;
    dq->capacity = new_capacity;
    dq->head = 0;
    return true;
}

static inline bool margo_ds_deque_push_back(margo_ds_deque_t *dq, const void *elem) {
    if (!dq || !elem || dq->elem_size == 0) {
        return false;
    }
    if (dq->size == dq->capacity) {
        size_t next_capacity = dq->capacity ? dq->capacity * 2 : 8;
        if (!margo_ds_deque_reserve(dq, next_capacity)) {
            return false;
        }
    }
    size_t tail = (dq->head + dq->size) % dq->capacity;
    memcpy((char *)dq->data + tail * dq->elem_size, elem, dq->elem_size);
    dq->size += 1;
    return true;
}

static inline bool margo_ds_deque_push_front(margo_ds_deque_t *dq, const void *elem) {
    if (!dq || !elem || dq->elem_size == 0) {
        return false;
    }
    if (dq->size == dq->capacity) {
        size_t next_capacity = dq->capacity ? dq->capacity * 2 : 8;
        if (!margo_ds_deque_reserve(dq, next_capacity)) {
            return false;
        }
    }
    dq->head = (dq->head + dq->capacity - 1) % dq->capacity;
    memcpy((char *)dq->data + dq->head * dq->elem_size, elem, dq->elem_size);
    dq->size += 1;
    return true;
}

static inline bool margo_ds_deque_pop_front(margo_ds_deque_t *dq, void *out_elem) {
    if (!dq || dq->size == 0) {
        return false;
    }
    if (out_elem) {
        memcpy(out_elem, (char *)dq->data + dq->head * dq->elem_size, dq->elem_size);
    }
    dq->head = (dq->head + 1) % dq->capacity;
    dq->size -= 1;
    return true;
}

static inline bool margo_ds_deque_pop_back(margo_ds_deque_t *dq, void *out_elem) {
    if (!dq || dq->size == 0) {
        return false;
    }
    size_t tail = (dq->head + dq->size - 1) % dq->capacity;
    if (out_elem) {
        memcpy(out_elem, (char *)dq->data + tail * dq->elem_size, dq->elem_size);
    }
    dq->size -= 1;
    return true;
}

