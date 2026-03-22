#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * @file diagnostics.h
 * @brief Declares the extremely small diagnostic structure leveraged by every
 *        frontend component. Even though the implementation is tiny, the
 *        detailed comments serve as living documentation for contributors.
 */

/**
 * @struct diagnostic
 * @brief Captures a single error condition produced during compilation.
 *
 * Only one message is stored at a time because our builder emits the first
 * error encountered. A fixed-size buffer is used to avoid heap allocations on
 * the critical diagnostic path.
 */
typedef struct diagnostic {
    bool has_error;   /**< Indicates whether the struct currently stores data. */
    size_t line;      /**< 1-based line number; zero when unknown. */
    char message[256];/**< Human-readable explanation of the failure. */
} diagnostic_t;

/**
 * @brief Reset the diagnostic buffer so future failures can reuse it.
 */
void diagnostic_clear(diagnostic_t *diag);

/**
 * @brief Store a formatted message and optional line number inside the buffer.
 */
void diagnostic_set(diagnostic_t *diag, size_t line, const char *fmt, ...);
