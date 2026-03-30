#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef int (*margo_ds_heap_compare_fn)(const void *lhs, const void *rhs);

typedef struct {
    void *data;
    size_t elem_size;
    size_t size;
    size_t capacity;
    margo_ds_heap_compare_fn cmp;
} margo_ds_heap_t;

static inline void margo_ds_heap_init(margo_ds_heap_t *heap,
                                      size_t elem_size,
                                      margo_ds_heap_compare_fn cmp) {
    if (!heap) {
        return;
    }
    heap->data = NULL;
    heap->elem_size = elem_size;
    heap->size = 0;
    heap->capacity = 0;
    heap->cmp = cmp;
}

static inline void margo_ds_heap_free(margo_ds_heap_t *heap) {
    if (!heap) {
        return;
    }
    free(heap->data);
    heap->data = NULL;
    heap->size = 0;
    heap->capacity = 0;
}

static inline void margo_ds_heap_swap(margo_ds_heap_t *heap, size_t a, size_t b) {
    if (!heap || a == b) {
        return;
    }
    unsigned char tmp[256];
    if (heap->elem_size <= sizeof(tmp)) {
        memcpy(tmp, (char *)heap->data + a * heap->elem_size, heap->elem_size);
        memcpy((char *)heap->data + a * heap->elem_size,
               (char *)heap->data + b * heap->elem_size,
               heap->elem_size);
        memcpy((char *)heap->data + b * heap->elem_size, tmp, heap->elem_size);
        return;
    }
    void *buf = malloc(heap->elem_size);
    if (!buf) {
        return;
    }
    memcpy(buf, (char *)heap->data + a * heap->elem_size, heap->elem_size);
    memcpy((char *)heap->data + a * heap->elem_size,
           (char *)heap->data + b * heap->elem_size,
           heap->elem_size);
    memcpy((char *)heap->data + b * heap->elem_size, buf, heap->elem_size);
    free(buf);
}

static inline bool margo_ds_heap_reserve(margo_ds_heap_t *heap, size_t new_capacity) {
    if (!heap || heap->elem_size == 0 || new_capacity <= heap->capacity) {
        return true;
    }
    void *next = realloc(heap->data, new_capacity * heap->elem_size);
    if (!next) {
        return false;
    }
    heap->data = next;
    heap->capacity = new_capacity;
    return true;
}

static inline void margo_ds_heap_sift_up(margo_ds_heap_t *heap, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        void *cur = (char *)heap->data + idx * heap->elem_size;
        void *par = (char *)heap->data + parent * heap->elem_size;
        if (heap->cmp(par, cur) <= 0) {
            break;
        }
        margo_ds_heap_swap(heap, parent, idx);
        idx = parent;
    }
}

static inline void margo_ds_heap_sift_down(margo_ds_heap_t *heap, size_t idx) {
    for (;;) {
        size_t left = idx * 2 + 1;
        size_t right = left + 1;
        size_t best = idx;
        if (left < heap->size) {
            void *l = (char *)heap->data + left * heap->elem_size;
            void *b = (char *)heap->data + best * heap->elem_size;
            if (heap->cmp(l, b) < 0) {
                best = left;
            }
        }
        if (right < heap->size) {
            void *r = (char *)heap->data + right * heap->elem_size;
            void *b = (char *)heap->data + best * heap->elem_size;
            if (heap->cmp(r, b) < 0) {
                best = right;
            }
        }
        if (best == idx) {
            break;
        }
        margo_ds_heap_swap(heap, idx, best);
        idx = best;
    }
}

static inline bool margo_ds_heap_push(margo_ds_heap_t *heap, const void *elem) {
    if (!heap || !elem || !heap->cmp || heap->elem_size == 0) {
        return false;
    }
    if (heap->size == heap->capacity) {
        size_t next_capacity = heap->capacity ? heap->capacity * 2 : 8;
        if (!margo_ds_heap_reserve(heap, next_capacity)) {
            return false;
        }
    }
    memcpy((char *)heap->data + heap->size * heap->elem_size, elem, heap->elem_size);
    heap->size += 1;
    margo_ds_heap_sift_up(heap, heap->size - 1);
    return true;
}

static inline bool margo_ds_heap_pop(margo_ds_heap_t *heap, void *out_elem) {
    if (!heap || heap->size == 0 || !heap->cmp) {
        return false;
    }
    if (out_elem) {
        memcpy(out_elem, heap->data, heap->elem_size);
    }
    heap->size -= 1;
    if (heap->size > 0) {
        memcpy(heap->data, (char *)heap->data + heap->size * heap->elem_size, heap->elem_size);
        margo_ds_heap_sift_down(heap, 0);
    }
    return true;
}

static inline void *margo_ds_heap_peek(margo_ds_heap_t *heap) {
    if (!heap || heap->size == 0) {
        return NULL;
    }
    return heap->data;
}

