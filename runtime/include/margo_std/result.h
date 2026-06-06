#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * @file margo_std/result.h
 * @brief Result<T, E> type for Margo error handling.
 *
 * Usage:
 *   Result(int, string) r = Ok(42)
 *   if is_ok(r) { print(unwrap(r), endl="\n") }
 */

#define Result(T, E) struct { bool _ok; union { T _ok_val; E _err_val; } _data; }

#define Ok(v) { ._ok = true, ._data._ok_val = (v) }

#define Err(e) { ._ok = false, ._data._err_val = (e) }

#define is_ok(r) ((r)._ok)

#define is_err(r) (!(r)._ok)

#define unwrap(r) ((r)._data._ok_val)

#define unwrap_err(r) ((r)._data._err_val)

#define unwrap_or(r, default_val) ((r)._ok ? (r)._data._ok_val : (default_val))

#define match_result(r, T, E, ok_name, ok_body, err_name, err_body) do { \
    if ((r)._ok) { \
        T ok_name = (r)._data._ok_val; \
        ok_body; \
    } else { \
        E err_name = (r)._data._err_val; \
        err_body; \
    } \
} while (0)
