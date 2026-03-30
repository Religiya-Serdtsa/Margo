#pragma once

#include "hashmap.h"

typedef struct {
    margo_ds_hashmap_t map;
} margo_ds_set_t;

static inline void margo_ds_set_init(margo_ds_set_t *set,
                                     size_t key_size,
                                     margo_ds_hash_fn hash_fn,
                                     margo_ds_key_eq_fn eq_fn) {
    if (!set) {
        return;
    }
    margo_ds_hashmap_init(&set->map, key_size, sizeof(unsigned char), hash_fn, eq_fn);
}

static inline void margo_ds_set_free(margo_ds_set_t *set) {
    if (!set) {
        return;
    }
    margo_ds_hashmap_free(&set->map);
}

static inline bool margo_ds_set_insert(margo_ds_set_t *set, const void *key) {
    if (!set) {
        return false;
    }
    unsigned char marker = 1;
    return margo_ds_hashmap_put(&set->map, key, &marker);
}

static inline bool margo_ds_set_contains(const margo_ds_set_t *set, const void *key) {
    if (!set) {
        return false;
    }
    return margo_ds_hashmap_get(&set->map, key) != NULL;
}

static inline bool margo_ds_set_erase(margo_ds_set_t *set, const void *key) {
    if (!set) {
        return false;
    }
    return margo_ds_hashmap_erase(&set->map, key);
}

