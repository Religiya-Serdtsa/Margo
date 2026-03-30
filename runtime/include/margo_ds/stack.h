#pragma once

#include "vector.h"

typedef struct {
    margo_ds_vector_t storage;
} margo_ds_stack_t;

static inline void margo_ds_stack_init(margo_ds_stack_t *stack, size_t elem_size) {
    if (!stack) {
        return;
    }
    margo_ds_vector_init(&stack->storage, elem_size);
}

static inline void margo_ds_stack_free(margo_ds_stack_t *stack) {
    if (!stack) {
        return;
    }
    margo_ds_vector_free(&stack->storage);
}

static inline bool margo_ds_stack_push(margo_ds_stack_t *stack, const void *elem) {
    return stack ? margo_ds_vector_push_back(&stack->storage, elem) : false;
}

static inline bool margo_ds_stack_pop(margo_ds_stack_t *stack, void *out_elem) {
    return stack ? margo_ds_vector_pop_back(&stack->storage, out_elem) : false;
}

static inline void *margo_ds_stack_top(margo_ds_stack_t *stack) {
    if (!stack || stack->storage.size == 0) {
        return NULL;
    }
    return margo_ds_vector_at(&stack->storage, stack->storage.size - 1);
}

