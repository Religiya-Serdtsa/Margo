#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/**
 * @file margo_std/vector.h
 * @brief Type-safe dynamic array (vector) for Margo.
 *
 * Usage in Margo:
 *   @import margo_std/vector
 *   vector(int) nums
 *   vec_init(&nums)
 *   vec_push(&nums, 10)
 *   vec_push(&nums, 20)
 *   for i in 0..vec_len(&nums) {
 *       print(vec_get(&nums, i), endl="\n")
 *   }
 *   vec_free(&nums)
 */

#define vector(T) struct { T *data; size_t len; size_t cap; }

#define vec_init(v) do { \
    (v)->data = NULL; \
    (v)->len = 0; \
    (v)->cap = 0; \
} while (0)

#define vec_reserve(v, n, T) do { \
    size_t _margo_new_cap = (n); \
    if (_margo_new_cap > (v)->cap) { \
        size_t _margo_want = _margo_new_cap < 8 ? 8 : _margo_new_cap; \
        T *_margo_new_data = (T *)realloc((v)->data, _margo_want * sizeof(T)); \
        if (_margo_new_data) { \
            (v)->data = _margo_new_data; \
            (v)->cap = _margo_want; \
        } \
    } \
} while (0)

#define vec_push(v, val, T) do { \
    if ((v)->len >= (v)->cap) { \
        size_t _margo_new_cap = (v)->cap ? (v)->cap * 2 : 8; \
        T *_margo_new_data = (T *)realloc((v)->data, _margo_new_cap * sizeof(T)); \
        if (_margo_new_data) { \
            (v)->data = _margo_new_data; \
            (v)->cap = _margo_new_cap; \
        } \
    } \
    if ((v)->data) { \
        (v)->data[(v)->len] = (val); \
        (v)->len++; \
    } \
} while (0)

#define vec_pop(v, T) ((v)->len > 0 ? (v)->data[--(v)->len] : (T){0})

#define vec_get(v, i) ((v)->data[i])

#define vec_set(v, i, val) do { (v)->data[i] = (val); } while (0)

#define vec_len(v) ((v)->len)

#define vec_cap(v) ((v)->cap)

#define vec_free(v) do { \
    free((v)->data); \
    (v)->data = NULL; \
    (v)->len = 0; \
    (v)->cap = 0; \
} while (0)

#define vec_clear(v) do { (v)->len = 0; } while (0)

#define vec_is_empty(v) ((v)->len == 0)

#define vec_last(v) ((v)->data[(v)->len - 1])

#define vec_foreach(v, T, it_name, body) do { \
    for (size_t _margo_i = 0; _margo_i < (v)->len; ++_margo_i) { \
        T it_name = (v)->data[_margo_i]; \
        body; \
    } \
} while (0)
