#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t (*margo_ds_hash_fn)(const void *key, size_t key_size);
typedef bool (*margo_ds_key_eq_fn)(const void *lhs, const void *rhs, size_t key_size);

typedef struct {
    unsigned char state; /* 0=empty, 1=used, 2=tombstone */
    void *key;
    void *value;
} margo_ds_hashmap_slot_t;

typedef struct {
    margo_ds_hashmap_slot_t *slots;
    size_t capacity;
    size_t size;
    size_t key_size;
    size_t value_size;
    margo_ds_hash_fn hash_fn;
    margo_ds_key_eq_fn eq_fn;
} margo_ds_hashmap_t;

static inline uint64_t margo_ds_hash_bytes(const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline bool margo_ds_key_eq_bytes(const void *lhs, const void *rhs, size_t size) {
    return memcmp(lhs, rhs, size) == 0;
}

static inline void margo_ds_hashmap_init(margo_ds_hashmap_t *map,
                                         size_t key_size,
                                         size_t value_size,
                                         margo_ds_hash_fn hash_fn,
                                         margo_ds_key_eq_fn eq_fn) {
    if (!map) {
        return;
    }
    map->slots = NULL;
    map->capacity = 0;
    map->size = 0;
    map->key_size = key_size;
    map->value_size = value_size;
    map->hash_fn = hash_fn ? hash_fn : margo_ds_hash_bytes;
    map->eq_fn = eq_fn ? eq_fn : margo_ds_key_eq_bytes;
}

static inline void margo_ds_hashmap_free(margo_ds_hashmap_t *map) {
    if (!map) {
        return;
    }
    if (map->slots) {
        for (size_t i = 0; i < map->capacity; ++i) {
            if (map->slots[i].state == 1) {
                free(map->slots[i].key);
                free(map->slots[i].value);
            }
        }
    }
    free(map->slots);
    map->slots = NULL;
    map->capacity = 0;
    map->size = 0;
}

static inline bool margo_ds_hashmap_rehash(margo_ds_hashmap_t *map, size_t new_capacity) {
    if (!map || map->key_size == 0 || map->value_size == 0 || new_capacity < 8) {
        return false;
    }
    margo_ds_hashmap_slot_t *next = calloc(new_capacity, sizeof(*next));
    if (!next) {
        return false;
    }

    margo_ds_hashmap_slot_t *old = map->slots;
    size_t old_capacity = map->capacity;

    map->slots = next;
    map->capacity = new_capacity;
    map->size = 0;

    for (size_t i = 0; i < old_capacity; ++i) {
        if (old[i].state != 1) {
            continue;
        }
        uint64_t h = map->hash_fn(old[i].key, map->key_size);
        size_t idx = (size_t)(h % map->capacity);
        while (map->slots[idx].state == 1) {
            idx = (idx + 1) % map->capacity;
        }
        map->slots[idx] = old[i];
        map->slots[idx].state = 1;
        map->size += 1;
    }

    free(old);
    return true;
}

static inline bool margo_ds_hashmap_put(margo_ds_hashmap_t *map, const void *key, const void *value) {
    if (!map || !key || !value || map->key_size == 0 || map->value_size == 0) {
        return false;
    }
    if (map->capacity == 0) {
        if (!margo_ds_hashmap_rehash(map, 16)) {
            return false;
        }
    }
    if ((map->size + 1) * 10 >= map->capacity * 7) {
        if (!margo_ds_hashmap_rehash(map, map->capacity * 2)) {
            return false;
        }
    }

    uint64_t h = map->hash_fn(key, map->key_size);
    size_t idx = (size_t)(h % map->capacity);
    size_t first_tombstone = SIZE_MAX;

    while (map->slots[idx].state != 0) {
        if (map->slots[idx].state == 2 && first_tombstone == SIZE_MAX) {
            first_tombstone = idx;
        } else if (map->slots[idx].state == 1 && map->eq_fn(map->slots[idx].key, key, map->key_size)) {
            memcpy(map->slots[idx].value, value, map->value_size);
            return true;
        }
        idx = (idx + 1) % map->capacity;
    }

    size_t target = (first_tombstone != SIZE_MAX) ? first_tombstone : idx;
    margo_ds_hashmap_slot_t *slot = &map->slots[target];
    if (slot->state != 1) {
        slot->key = malloc(map->key_size);
        slot->value = malloc(map->value_size);
        if (!slot->key || !slot->value) {
            free(slot->key);
            free(slot->value);
            slot->key = NULL;
            slot->value = NULL;
            return false;
        }
        map->size += 1;
    }
    memcpy(slot->key, key, map->key_size);
    memcpy(slot->value, value, map->value_size);
    slot->state = 1;
    return true;
}

static inline void *margo_ds_hashmap_get(const margo_ds_hashmap_t *map, const void *key) {
    if (!map || !map->slots || !key || map->capacity == 0) {
        return NULL;
    }
    uint64_t h = map->hash_fn(key, map->key_size);
    size_t idx = (size_t)(h % map->capacity);

    while (map->slots[idx].state != 0) {
        if (map->slots[idx].state == 1 && map->eq_fn(map->slots[idx].key, key, map->key_size)) {
            return map->slots[idx].value;
        }
        idx = (idx + 1) % map->capacity;
    }
    return NULL;
}

static inline bool margo_ds_hashmap_erase(margo_ds_hashmap_t *map, const void *key) {
    if (!map || !map->slots || !key || map->capacity == 0) {
        return false;
    }
    uint64_t h = map->hash_fn(key, map->key_size);
    size_t idx = (size_t)(h % map->capacity);

    while (map->slots[idx].state != 0) {
        if (map->slots[idx].state == 1 && map->eq_fn(map->slots[idx].key, key, map->key_size)) {
            free(map->slots[idx].key);
            free(map->slots[idx].value);
            map->slots[idx].key = NULL;
            map->slots[idx].value = NULL;
            map->slots[idx].state = 2;
            map->size -= 1;
            return true;
        }
        idx = (idx + 1) % map->capacity;
    }
    return false;
}

