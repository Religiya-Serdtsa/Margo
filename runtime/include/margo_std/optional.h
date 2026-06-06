#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * @file margo_std/optional.h
 * @brief Optional<T> type for Margo.
 *
 * Usage:
 *   Optional(int) maybe = Some(42)
 *   if is_some(maybe) { print(unwrap(maybe), endl="\n") }
 */

#define Optional(T) struct { bool _some; T _value; }

#define Some(v) { ._some = true, ._value = (v) }

#define None { ._some = false }

#define is_some(o) ((o)._some)

#define is_none(o) (!(o)._some)

#define opt_unwrap(o) ((o)._value)

#define opt_unwrap_or(o, default_val) ((o)._some ? (o)._value : (default_val))

#define map_opt(o, T, expr) ({ \
    Optional(T) _margo_result = None; \
    if ((o)._some) { \
        __typeof__(expr) _margo_mapped = (expr); \
        _margo_result = (_margo_result) { ._some = true, ._value = _margo_mapped }; \
    } \
    _margo_result; \
})
