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
} margo_ds_queue_t;

static inline void margo_ds_queue_init(margo_ds_queue_t *q, size_t elem_size) {
    if (!q) {
        return;
    }
    q->data = NULL;
    q->elem_size = elem_size;
    q->size = 0;
    q->capacity = 0;
    q->head = 0;
}

static inline void margo_ds_queue_free(margo_ds_queue_t *q) {
    if (!q) {
        return;
    }
    free(q->data);
    q->data = NULL;
    q->size = 0;
    q->capacity = 0;
    q->head = 0;
}

static inline bool margo_ds_queue_reserve(margo_ds_queue_t *q, size_t new_capacity) {
    if (!q || q->elem_size == 0 || new_capacity <= q->capacity) {
        return true;
    }
    void *next = malloc(new_capacity * q->elem_size);
    if (!next) {
        return false;
    }
    for (size_t i = 0; i < q->size; ++i) {
        size_t idx = (q->head + i) % (q->capacity ? q->capacity : 1);
        memcpy((char *)next + i * q->elem_size,
               (char *)q->data + idx * q->elem_size,
               q->elem_size);
    }
    free(q->data);
    q->data = next;
    q->capacity = new_capacity;
    q->head = 0;
    return true;
}

static inline bool margo_ds_queue_push(margo_ds_queue_t *q, const void *elem) {
    if (!q || !elem || q->elem_size == 0) {
        return false;
    }
    if (q->size == q->capacity) {
        size_t next_capacity = q->capacity ? q->capacity * 2 : 8;
        if (!margo_ds_queue_reserve(q, next_capacity)) {
            return false;
        }
    }
    size_t tail = (q->head + q->size) % q->capacity;
    memcpy((char *)q->data + tail * q->elem_size, elem, q->elem_size);
    q->size += 1;
    return true;
}

static inline bool margo_ds_queue_pop(margo_ds_queue_t *q, void *out_elem) {
    if (!q || q->size == 0) {
        return false;
    }
    if (out_elem) {
        memcpy(out_elem, (char *)q->data + q->head * q->elem_size, q->elem_size);
    }
    q->head = (q->head + 1) % q->capacity;
    q->size -= 1;
    return true;
}

static inline void *margo_ds_queue_front(margo_ds_queue_t *q) {
    if (!q || q->size == 0) {
        return NULL;
    }
    return (char *)q->data + q->head * q->elem_size;
}

