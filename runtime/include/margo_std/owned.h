#pragma once

#include <stddef.h>

/* owned_new/owned_array use Margo's libttak-backed alloc and are tracked by
 * the transpiler for scope-exit and early-return release. */
#define owned(T) T *
#define owned_new(T) ((T *)alloc(sizeof(T)))
#define owned_array(T, count) ((T *)alloc((size_t)(count) * sizeof(T)))
