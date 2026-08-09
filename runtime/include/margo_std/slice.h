#pragma once

#include <stddef.h>
#include <stdlib.h>

typedef struct {
    void   *data;
    size_t  len;
    size_t  elem_size;
} margo_slice_t;

#define slice(T) margo_slice_t
#define slice_from(T, array) ((margo_slice_t){ \
    .data = (array), .len = sizeof(array) / sizeof((array)[0]), .elem_size = sizeof(T) })
#define slice_make(T, pointer, count) ((margo_slice_t){ \
    .data = (pointer), .len = (count), .elem_size = sizeof(T) })
#define slice_len(value) ((value).len)
#define slice_data(T, value) ((T *)((value).data))

static inline void *margo_slice_at(margo_slice_t value, size_t index, size_t expected_size) {
    if (value.elem_size != expected_size || index >= value.len) {
        abort();
    }
    return (unsigned char *)value.data + index * value.elem_size;
}

#define slice_at(T, value, index) (*(T *)margo_slice_at((value), (index), sizeof(T)))
