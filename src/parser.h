#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "diagnostics.h"
#include "lexer.h"

/**
 * @file parser.h
 * @brief Structures describing the syntactic sugar constructs handled by the
 *        transpiler.
 */

/**
 * @brief Supported prefixes for `@import` directives.
 *
 * New kinds added for self-hosting support:
 *  - IMPORT_KIND_CPP     maps `@import c++/...` to a stub comment (C++ ABI
 *                        support is a future milestone).
 *  - IMPORT_KIND_GODMODE enables all common standard C headers in one shot.
 *  - IMPORT_KIND_STD_STRING  maps `@import std/string`  -> <string.h>
 *  - IMPORT_KIND_STD_MATH    maps `@import std/math`    -> <math.h>
 *  - IMPORT_KIND_STD_STDLIB  maps `@import std/stdlib`  -> <stdlib.h>
 *  - IMPORT_KIND_STD_TIME    maps `@import std/time`    -> <time.h>
 *  - IMPORT_KIND_STD_ASSERT  maps `@import std/assert`  -> <assert.h>
 *  - IMPORT_KIND_STD_ERRNO   maps `@import std/errno`   -> <errno.h>
 *  - IMPORT_KIND_STD_FILE    maps `@import std/file`    -> <stdio.h>
 *  - IMPORT_KIND_THREADS_CORE maps `@import threads/core` -> "threads/core.h"
 *  - IMPORT_KIND_PROCESS_CORE maps `@import process` or `@import process/core`
 *                              into "process/core.h"
 *  - IMPORT_KIND_MATRIX_CORE maps `@import matrix/core` into the matrix runtime
 *                              utilities header.
 */
typedef enum {
    IMPORT_KIND_C,
    IMPORT_KIND_STD,
    IMPORT_KIND_CPP,
    IMPORT_KIND_GODMODE,
    IMPORT_KIND_STD_STRING,
    IMPORT_KIND_STD_MATH,
    IMPORT_KIND_STD_STDLIB,
    IMPORT_KIND_STD_TIME,
    IMPORT_KIND_STD_ASSERT,
    IMPORT_KIND_STD_ERRNO,
    IMPORT_KIND_STD_FILE,
    IMPORT_KIND_THREADS_CORE,
    IMPORT_KIND_PROCESS_CORE,
    IMPORT_KIND_MATRIX_CORE,
} import_kind_t;

/**
 * @brief Parsed representation of an `@import` directive.
 */
typedef struct {
    import_kind_t kind;
    char *target;
    size_t start_offset;
    size_t end_offset;
    size_t next_index;
} import_directive_t;

/**
 * @brief Captures the three sections of the custom `for` header syntax.
 */
typedef struct {
    bool has_initializer;
    char *initializer;
    char *condition;
    char *increment;
    bool has_loop_identifier;
    char loop_identifier[64];
    char *trailing_ws;
    size_t start_offset;
    size_t block_offset;
    size_t next_index;
} for_header_t;

/**
 * @brief Captures offsets for parenthesis-free conditional headers such as
 *        `if` and `while`.
 */
typedef struct {
    char *condition;
    size_t start_offset;
    size_t condition_end_offset;
    size_t body_offset;
    size_t next_index;
} condition_header_t;

typedef condition_header_t if_header_t;
typedef condition_header_t while_header_t;

bool parser_parse_import(const char *source,
                         const token_buffer_t *tokens,
                         size_t start_index,
                         import_directive_t *out,
                         diagnostic_t *diag);
void parser_free_import(import_directive_t *dir);

bool parser_parse_for_header(const char *source,
                             const token_buffer_t *tokens,
                             size_t start_index,
                             for_header_t *out,
                             diagnostic_t *diag);
void parser_free_for_header(for_header_t *header);

bool parser_parse_if_header(const char *source,
                            const token_buffer_t *tokens,
                            size_t start_index,
                            if_header_t *out,
                            diagnostic_t *diag);
void parser_free_if_header(if_header_t *header);

bool parser_parse_while_header(const char *source,
                               const token_buffer_t *tokens,
                               size_t start_index,
                               while_header_t *out,
                               diagnostic_t *diag);
void parser_free_while_header(while_header_t *header);
