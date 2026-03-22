#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "diagnostics.h"

/**
 * @file lexer.h
 * @brief Token definitions used by the experimental parser/transpiler.
 */

/**
 * @brief Enumerates all token kinds recognized by the lexer.
 */
typedef enum {
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_CHAR,
    TOKEN_NEWLINE,
    TOKEN_SYMBOL,
    TOKEN_AT,
    TOKEN_EOF,
} token_kind_t;

/**
 * @brief Represents a token along with the slice of source text that produced
 *        it. The lexer keeps pointers into the original buffer to avoid
 *        allocations while still enabling descriptive diagnostics.
 */
typedef struct {
    token_kind_t kind;
    const char *lexeme;
    size_t length;
    size_t offset;
    size_t line;
} token_t;

/**
 * @brief Convenient dynamic array wrapper for storing tokens.
 */
typedef struct {
    token_t *items;
    size_t count;
    size_t capacity;
} token_buffer_t;

/**
 * @brief Convert the raw source into a stream of tokens.
 */
bool lexer_tokenize(const char *source, size_t length, token_buffer_t *out, diagnostic_t *diag);
/**
 * @brief Release the memory owned by a token buffer.
 */
void lexer_free(token_buffer_t *buffer);

/**
 * @brief Utility helpers used by the parser when inspecting tokens.
 */
bool token_is_identifier(const token_t *tok, const char *literal);
bool token_is_symbol(const token_t *tok, char symbol);
