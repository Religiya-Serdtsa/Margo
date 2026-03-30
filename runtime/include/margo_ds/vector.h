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
} margo_ds_vector_t;

static inline void margo_ds_vector_init(margo_ds_vector_t *vec, size_t elem_size) {
    if (!vec) {
        return;
    }
    vec->data = NULL;
    vec->elem_size = elem_size;
    vec->size = 0;
    vec->capacity = 0;
}

static inline void margo_ds_vector_free(margo_ds_vector_t *vec) {
    if (!vec) {
        return;
    }
    free(vec->data);
    vec->data = NULL;
    vec->size = 0;
    vec->capacity = 0;
}

static inline bool margo_ds_vector_reserve(margo_ds_vector_t *vec, size_t new_capacity) {
    if (!vec || vec->elem_size == 0 || new_capacity <= vec->capacity) {
        return true;
    }
    void *next = realloc(vec->data, new_capacity * vec->elem_size);
    if (!next) {
        return false;
    }
    vec->data = next;
    vec->capacity = new_capacity;
    return true;
}

static inline bool margo_ds_vector_push_back(margo_ds_vector_t *vec, const void *elem) {
    if (!vec || !elem || vec->elem_size == 0) {
        return false;
    }
    if (vec->size == vec->capacity) {
        size_t next_capacity = vec->capacity ? vec->capacity * 2 : 8;
        if (!margo_ds_vector_reserve(vec, next_capacity)) {
            return false;
        }
    }
    memcpy((char *)vec->data + (vec->size * vec->elem_size), elem, vec->elem_size);
    vec->size += 1;
    return true;
}

static inline bool margo_ds_vector_pop_back(margo_ds_vector_t *vec, void *out_elem) {
    if (!vec || vec->size == 0) {
        return false;
    }
    vec->size -= 1;
    if (out_elem) {
        memcpy(out_elem, (char *)vec->data + (vec->size * vec->elem_size), vec->elem_size);
    }
    return true;
}

static inline void *margo_ds_vector_at(margo_ds_vector_t *vec, size_t index) {
    if (!vec || index >= vec->size) {
        return NULL;
    }
    return (char *)vec->data + (index * vec->elem_size);
}

static inline const void *margo_ds_vector_at_const(const margo_ds_vector_t *vec, size_t index) {
    if (!vec || index >= vec->size) {
        return NULL;
    }
    return (const char *)vec->data + (index * vec->elem_size);
}

