#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "diagnostics.h"

/**
 * @file transpiler.h
 * @brief Public interface for the parser-backed transpiler.
 */

/**
 * @brief Convert a `.margo` source file into pure C stored inside a heap buffer.
 *
 * The caller takes ownership of `*buffer_out` and must `free` it. Diagnostics
 * are populated whenever lexing/parsing fails.
 */
bool margo_transpile_to_buffer(const char *input_path, char **buffer_out, size_t *size_out, diagnostic_t *diag);
