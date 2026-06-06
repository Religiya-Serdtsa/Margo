#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @file margo_std/hashmap.h
 * @brief Simple open-addressing hash map for Margo.
 *
 * Keys are always strings (char*). Values are typed via macro.
 *
 * Usage:
 *   hashmap(int) scores
 *   hm_init(&scores)
 *   hm_put(&scores, "alice", 100)
 *   hm_put(&scores, "bob", 200)
 *   auto alice_score = hm_get(&scores, "alice", 0)
 *   print(alice_score, endl="\n")
 *   hm_free(&scores)
 */

#define MARGO_HM_TOMB ((const char *)1)

#define hashmap(V) struct { \
    char **keys; \
    V *values; \
    size_t len; \
    size_t cap; \
}

static inline size_t margo_hm_hash(const char *key, size_t cap) {
    uint64_t h = 14695981039346656037ULL;
    while (*key) {
        h ^= (unsigned char)(*key);
        h *= 1099511628211ULL;
        key++;
    }
    return (size_t)(h & (cap - 1));
}

#define hm_init(m) do { \
    (m)->keys = NULL; \
    (m)->values = NULL; \
    (m)->len = 0; \
    (m)->cap = 0; \
} while (0)

#define hm__grow(m, V) do { \
    size_t _margo_old_cap = (m)->cap; \
    size_t _margo_new_cap = _margo_old_cap ? _margo_old_cap * 2 : 16; \
    char **_margo_new_keys = (char **)calloc(_margo_new_cap, sizeof(char *)); \
    V *_margo_new_values = (V *)calloc(_margo_new_cap, sizeof(V)); \
    if (_margo_new_keys && _margo_new_values) { \
        for (size_t _margo_i = 0; _margo_i < _margo_old_cap; ++_margo_i) { \
            if ((m)->keys[_margo_i] && (m)->keys[_margo_i] != MARGO_HM_TOMB) { \
                size_t _margo_h = margo_hm_hash((m)->keys[_margo_i], _margo_new_cap); \
                while (_margo_new_keys[_margo_h]) { \
                    _margo_h = (_margo_h + 1) & (_margo_new_cap - 1); \
                } \
                _margo_new_keys[_margo_h] = (m)->keys[_margo_i]; \
                _margo_new_values[_margo_h] = (m)->values[_margo_i]; \
            } \
        } \
        free((m)->keys); \
        free((m)->values); \
        (m)->keys = _margo_new_keys; \
        (m)->values = _margo_new_values; \
        (m)->cap = _margo_new_cap; \
    } \
} while (0)

#define hm_put(m, key, val, V) do { \
    if ((m)->len >= ((m)->cap * 3) / 4) { \
        hm__grow(m, V); \
    } \
    if ((m)->cap == 0) { \
        hm__grow(m, V); \
    } \
    if ((m)->cap > 0) { \
        size_t _margo_h = margo_hm_hash(key, (m)->cap); \
        size_t _margo_tomb_idx = (size_t)-1; \
        while ((m)->keys[_margo_h] && (m)->keys[_margo_h] != MARGO_HM_TOMB) { \
            if (strcmp((m)->keys[_margo_h], key) == 0) { \
                (m)->values[_margo_h] = (val); \
                break; \
            } \
            _margo_h = (_margo_h + 1) & ((m)->cap - 1); \
        } \
        if ((m)->keys[_margo_h] == NULL || (m)->keys[_margo_h] == MARGO_HM_TOMB) { \
            (m)->keys[_margo_h] = strdup(key); \
            (m)->values[_margo_h] = (val); \
            (m)->len++; \
        } \
    } \
} while (0)

#define hm_get(m, key, default_val) ({ \
    __typeof__((m)->values[0]) _margo_result = (default_val); \
    if ((m)->cap > 0) { \
        size_t _margo_h = margo_hm_hash(key, (m)->cap); \
        for (size_t _margo_probe = 0; _margo_probe < (m)->cap; ++_margo_probe) { \
            if ((m)->keys[_margo_h] == NULL) break; \
            if ((m)->keys[_margo_h] != MARGO_HM_TOMB && strcmp((m)->keys[_margo_h], key) == 0) { \
                _margo_result = (m)->values[_margo_h]; \
                break; \
            } \
            _margo_h = (_margo_h + 1) & ((m)->cap - 1); \
        } \
    } \
    _margo_result; \
})

#define hm_remove(m, key) do { \
    if ((m)->cap > 0) { \
        size_t _margo_h = margo_hm_hash(key, (m)->cap); \
        for (size_t _margo_probe = 0; _margo_probe < (m)->cap; ++_margo_probe) { \
            if ((m)->keys[_margo_h] == NULL) break; \
            if ((m)->keys[_margo_h] != MARGO_HM_TOMB && strcmp((m)->keys[_margo_h], key) == 0) { \
                free((m)->keys[_margo_h]); \
                (m)->keys[_margo_h] = (char *)MARGO_HM_TOMB; \
                (m)->len--; \
                break; \
            } \
            _margo_h = (_margo_h + 1) & ((m)->cap - 1); \
        } \
    } \
} while (0)

#define hm_contains(m, key) ({ \
    bool _margo_found = false; \
    if ((m)->cap > 0) { \
        size_t _margo_h = margo_hm_hash(key, (m)->cap); \
        for (size_t _margo_probe = 0; _margo_probe < (m)->cap; ++_margo_probe) { \
            if ((m)->keys[_margo_h] == NULL) break; \
            if ((m)->keys[_margo_h] != MARGO_HM_TOMB && strcmp((m)->keys[_margo_h], key) == 0) { \
                _margo_found = true; \
                break; \
            } \
            _margo_h = (_margo_h + 1) & ((m)->cap - 1); \
        } \
    } \
    _margo_found; \
})

#define hm_len(m) ((m)->len)

#define hm_is_empty(m) ((m)->len == 0)

#define hm_free(m) do { \
    if ((m)->keys) { \
        for (size_t _margo_i = 0; _margo_i < (m)->cap; ++_margo_i) { \
            if ((m)->keys[_margo_i] && (m)->keys[_margo_i] != MARGO_HM_TOMB) { \
                free((m)->keys[_margo_i]); \
            } \
        } \
        free((m)->keys); \
        free((m)->values); \
    } \
    (m)->keys = NULL; \
    (m)->values = NULL; \
    (m)->len = 0; \
    (m)->cap = 0; \
} while (0)
