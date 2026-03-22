#include "lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file lexer.c
 * @brief Minimal tokenizer that feeds the parser. Every helper has detailed
 *        comments because the lexer silently dictates the grammar we can
 *        support; documenting the intent avoids future accidental regressions.
 */

typedef struct {
    const char *src;
    size_t length;
    size_t pos;
    size_t line;
} lexer_state_t;

/**
 * @brief Append a token to the dynamic buffer, growing it when necessary.
 */
static bool append_token(token_buffer_t *buffer, token_t token) {
    if (buffer->count == buffer->capacity) {
        size_t new_capacity = buffer->capacity ? buffer->capacity * 2 : 64;
        token_t *new_items = realloc(buffer->items, new_capacity * sizeof(token_t));
        if (!new_items) {
            return false;
        }
        buffer->items = new_items;
        buffer->capacity = new_capacity;
    }
    buffer->items[buffer->count++] = token;
    return true;
}

/**
 * @brief Consume characters until the end of the current line.
 */
static void skip_line_comment(lexer_state_t *lex) {
    while (lex->pos < lex->length) {
        char c = lex->src[lex->pos];
        if (c == '\n') {
            break;
        }
        lex->pos++;
    }
}

/**
 * @brief Consume a classic C-style block comment (slash-star ... star-slash).
 */
static bool skip_block_comment(lexer_state_t *lex, diagnostic_t *diag) {
    while (lex->pos + 1 < lex->length) {
        char c = lex->src[lex->pos];
        char next = lex->src[lex->pos + 1];
        if (c == '*' && next == '/') {
            lex->pos += 2;
            return true;
        }
        if (c == '\n') {
            lex->line++;
        }
        lex->pos++;
    }
    diagnostic_set(diag, lex->line, "unterminated block comment");
    return false;
}

/**
 * @brief Emit a newline token for `\n` or Windows-style `\r\n` sequences.
 */
static bool emit_newline(lexer_state_t *lex, token_buffer_t *out) {
    size_t start = lex->pos;
    size_t length = 1;
    if (lex->src[lex->pos] == '\r' && lex->pos + 1 < lex->length && lex->src[lex->pos + 1] == '\n') {
        length = 2;
    }
    lex->pos += length;
    token_t tok = {
        .kind = TOKEN_NEWLINE,
        .lexeme = lex->src + start,
        .length = length,
        .offset = start,
        .line = lex->line,
    };
    lex->line++;
    return append_token(out, tok);
}

/**
 * @brief Emit an identifier token starting at the current cursor.
 */
static bool emit_identifier(lexer_state_t *lex, token_buffer_t *out) {
    size_t start = lex->pos;
    lex->pos++;
    while (lex->pos < lex->length) {
        char c = lex->src[lex->pos];
        if (!(isalnum((unsigned char)c) || c == '_')) {
            break;
        }
        lex->pos++;
    }
    token_t tok = {
        .kind = TOKEN_IDENTIFIER,
        .lexeme = lex->src + start,
        .length = lex->pos - start,
        .offset = start,
        .line = lex->line,
    };
    return append_token(out, tok);
}

/**
 * @brief Emit a number token, allowing underscores and hex digits.
 */
static bool emit_number(lexer_state_t *lex, token_buffer_t *out) {
    size_t start = lex->pos;
    lex->pos++;
    while (lex->pos < lex->length) {
        char c = lex->src[lex->pos];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) {
            break;
        }
        lex->pos++;
    }
    token_t tok = {
        .kind = TOKEN_NUMBER,
        .lexeme = lex->src + start,
        .length = lex->pos - start,
        .offset = start,
        .line = lex->line,
    };
    return append_token(out, tok);
}

/**
 * @brief Emit a string or character literal and ensure it terminates properly.
 */
static bool emit_string_like(lexer_state_t *lex, token_buffer_t *out, diagnostic_t *diag, char delimiter, token_kind_t kind) {
    size_t start = lex->pos;
    lex->pos++;
    while (lex->pos < lex->length) {
        char c = lex->src[lex->pos];
        if (c == delimiter) {
            lex->pos++;
            token_t tok = {
                .kind = kind,
                .lexeme = lex->src + start,
                .length = lex->pos - start,
                .offset = start,
                .line = lex->line,
            };
            return append_token(out, tok);
        }
        if (c == '\\' && lex->pos + 1 < lex->length) {
            lex->pos += 2;
            continue;
        }
        if (c == '\n') {
            lex->line++;
        }
        lex->pos++;
    }
    diagnostic_set(diag, lex->line, "unterminated literal");
    return false;
}

/**
 * @brief Emit a generic symbol token of the requested width.
 */
static bool emit_symbol(lexer_state_t *lex, token_buffer_t *out, token_kind_t kind, size_t width) {
    size_t start = lex->pos;
    lex->pos += width;
    token_t tok = {
        .kind = kind,
        .lexeme = lex->src + start,
        .length = width,
        .offset = start,
        .line = lex->line,
    };
    return append_token(out, tok);
}

/**
 * @brief Tokenize the provided source text.
 */
bool lexer_tokenize(const char *source, size_t length, token_buffer_t *out, diagnostic_t *diag) {
    memset(out, 0, sizeof(*out));
    lexer_state_t lex = {.src = source, .length = length, .pos = 0, .line = 1};
    while (lex.pos < lex.length) {
        char c = lex.src[lex.pos];
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
            lex.pos++;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!emit_newline(&lex, out)) {
                diagnostic_set(diag, lex.line, "out of memory while recording newline tokens");
                lexer_free(out);
                return false;
            }
            continue;
        }
        if (c == '/' && lex.pos + 1 < lex.length) {
            char next = lex.src[lex.pos + 1];
            if (next == '/') {
                lex.pos += 2;
                skip_line_comment(&lex);
                continue;
            }
            if (next == '*') {
                lex.pos += 2;
                if (!skip_block_comment(&lex, diag)) {
                    lexer_free(out);
                    return false;
                }
                continue;
            }
        }
        if (c == '@') {
            if (!emit_symbol(&lex, out, TOKEN_AT, 1)) {
                diagnostic_set(diag, lex.line, "out of memory while tokenizing");
                lexer_free(out);
                return false;
            }
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            if (!emit_identifier(&lex, out)) {
                diagnostic_set(diag, lex.line, "out of memory while tokenizing identifiers");
                lexer_free(out);
                return false;
            }
            continue;
        }
        if (isdigit((unsigned char)c)) {
            if (!emit_number(&lex, out)) {
                diagnostic_set(diag, lex.line, "out of memory while tokenizing numbers");
                lexer_free(out);
                return false;
            }
            continue;
        }
        if (c == '"') {
            if (!emit_string_like(&lex, out, diag, '"', TOKEN_STRING)) {
                lexer_free(out);
                return false;
            }
            continue;
        }
        if (c == '\'') {
            if (!emit_string_like(&lex, out, diag, '\'', TOKEN_CHAR)) {
                lexer_free(out);
                return false;
            }
            continue;
        }
        if (!emit_symbol(&lex, out, TOKEN_SYMBOL, 1)) {
            diagnostic_set(diag, lex.line, "out of memory while tokenizing symbols");
            lexer_free(out);
            return false;
        }
    }
    token_t eof = {
        .kind = TOKEN_EOF,
        .lexeme = source + length,
        .length = 0,
        .offset = length,
        .line = lex.line,
    };
    if (!append_token(out, eof)) {
        diagnostic_set(diag, lex.line, "out of memory while finalizing tokens");
        lexer_free(out);
        return false;
    }
    return true;
}

/**
 * @brief Release the heap memory backing the token buffer.
 */
void lexer_free(token_buffer_t *buffer) {
    if (!buffer) {
        return;
    }
    free(buffer->items);
    buffer->items = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

/**
 * @brief Convenience predicate for matching identifiers.
 */
bool token_is_identifier(const token_t *tok, const char *literal) {
    if (!tok || tok->kind != TOKEN_IDENTIFIER) {
        return false;
    }
    size_t len = strlen(literal);
    if (tok->length != len) {
        return false;
    }
    return strncmp(tok->lexeme, literal, len) == 0;
}

/**
 * @brief Convenience predicate for matching single-character symbols.
 */
bool token_is_symbol(const token_t *tok, char symbol) {
    if (!tok || tok->kind != TOKEN_SYMBOL) {
        return false;
    }
    if (tok->length != 1) {
        return false;
    }
    return tok->lexeme[0] == symbol;
}
