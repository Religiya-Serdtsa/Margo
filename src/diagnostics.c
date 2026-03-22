#include "diagnostics.h"

#include <stdarg.h>
#include <stdio.h>

/**
 * @file diagnostics.c
 * @brief Implements the ultra-minimal diagnostic helper that every subsystem
 *        uses to report failures while keeping the surrounding code clean. All
 *        functions are intentionally tiny, but they receive verbose comments so
 *        future contributors know exactly why they exist and how to use them.
 */

/**
 * @brief Reset the diagnostic structure so downstream code can reuse it.
 *
 * The transpiler builds complicated error messages and often bubbles the same
 * diagnostic buffer through multiple helpers. Calling this function ensures the
 * buffer does not contain stale messages. We explicitly guard against NULL so
 * callers can conditionally reset without repeated checks.
 *
 * @param diag Pointer to the diagnostic buffer that should be cleared.
 */
void diagnostic_clear(diagnostic_t *diag) {
    if (!diag) {
        return;
    }
    diag->has_error = false;
    diag->line = 0;
    diag->message[0] = '\0';
}

/**
 * @brief Store a formatted message along with the line number that triggered it.
 *
 * This helper mirrors `fprintf` semantics but specifically targets the fixed
 * diagnostic buffer. We intentionally do not append newlines or additional
 * decorations because every call site has different needs.
 *
 * @param diag Destination buffer; may be NULL to disable diagnostics entirely.
 * @param line 1-based source line number associated with the error.
 * @param fmt  printf-style format string.
 * @param ...  Additional arguments consumed by the format string.
 */
void diagnostic_set(diagnostic_t *diag, size_t line, const char *fmt, ...) {
    if (!diag) {
        return;
    }
    diag->has_error = true;
    diag->line = line;
    va_list args;
    va_start(args, fmt);
    vsnprintf(diag->message, sizeof(diag->message), fmt, args);
    va_end(args);
}
