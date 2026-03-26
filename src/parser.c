#include "parser.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file parser.c
 * @brief Hand-rolled helpers that understand the limited syntax sugar supported
 *        by the transpiler. The generous documentation is intentional: the
 *        parser doubles as a spec for the concept code, so we explain the
 *        rationale for every transformation.
 */

/**
 * @brief Portable whitespace predicate that avoids locale surprises.
 */
static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/**
 * @brief `strdup` alternative that keeps us in C17 land.
 */
static char *dup_string(const char *src) {
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, src, len + 1);
    return copy;
}

/**
 * @brief Copy an inclusive-exclusive range from the source buffer.
 */
static char *copy_range(const char *src, size_t start, size_t end) {
    if (end < start) {
        end = start;
    }
    size_t len = end - start;
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    if (len) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
    return out;
}

/**
 * @brief Copy a range after stripping surrounding whitespace.
 */
static char *copy_trimmed(const char *src, size_t start, size_t end, size_t *trimmed_end) {
    while (start < end && is_space(src[start])) {
        start++;
    }
    while (end > start && is_space(src[end - 1])) {
        end--;
    }
    if (trimmed_end) {
        *trimmed_end = end;
    }
    return copy_range(src, start, end);
}

/**
 * @brief Remove all whitespace characters from the provided slice.
 */
static char *copy_without_spaces(const char *text) {
    size_t len = strlen(text);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t w = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!is_space(text[i])) {
            out[w++] = text[i];
        }
    }
    out[w] = '\0';
    return out;
}

/**
 * @brief Guess the implicit loop variable name from the initializer segment.
 */
static bool detect_loop_identifier(const char *expr,
                                   char *buffer,
                                   size_t buffer_len,
                                   bool *has_explicit_type) {
    if (!expr) {
        return false;
    }
    size_t len = strlen(expr);
    size_t eq_pos = len;
    for (size_t i = 0; i < len; ++i) {
        if (expr[i] == '=') {
            if (i + 1 < len && expr[i + 1] == '=') {
                continue;
            }
            eq_pos = i;
            break;
        }
    }
    if (eq_pos == len) {
        return false;
    }
    size_t end = eq_pos;
    while (end > 0 && is_space(expr[end - 1])) {
        end--;
    }
    size_t start = end;
    while (start > 0) {
        char c = expr[start - 1];
        if (!(isalnum((unsigned char)c) || c == '_')) {
            break;
        }
        start--;
    }
    if (start == end) {
        return false;
    }
    size_t ident_len = end - start;
    if (ident_len + 1 > buffer_len) {
        ident_len = buffer_len - 1;
    }
    memcpy(buffer, expr + start, ident_len);
    buffer[ident_len] = '\0';
    if (has_explicit_type) {
        bool explicit_type = false;
        for (size_t i = 0; i < start; ++i) {
            if (!isspace((unsigned char)expr[i])) {
                explicit_type = true;
                break;
            }
        }
        *has_explicit_type = explicit_type;
    }
    return true;
}

/**
 * @brief Prepend the implicit loop identifier to abbreviated conditions.
 */
static char *rewrite_condition(const char *expr, const char *loop_ident) {
    if (!expr) {
        return NULL;
    }
    if (!loop_ident || !*loop_ident) {
        return dup_string(expr);
    }
    const char *trim = expr;
    while (*trim && is_space(*trim)) {
        trim++;
    }
    if (*trim == '\0') {
        return dup_string(expr);
    }
    if (*trim == '<' || *trim == '>' || *trim == '=' || *trim == '!') {
        size_t needed = strlen(loop_ident) + 1 + strlen(trim) + 1;
        char *out = malloc(needed);
        if (!out) {
            return NULL;
        }
        snprintf(out, needed, "%s %s", loop_ident, trim);
        return out;
    }
    if (*trim == '.') {
        size_t needed = strlen(loop_ident) + strlen(trim) + 1;
        char *out = malloc(needed);
        if (!out) {
            return NULL;
        }
        snprintf(out, needed, "%s%s", loop_ident, trim);
        return out;
    }
    return dup_string(expr);
}

/**
 * @brief Small helper for increment rewrites.
 */
static bool has_prefix(const char *expr, const char *prefix) {
    return strncmp(expr, prefix, strlen(prefix)) == 0;
}

/**
 * @brief Keywords that cannot appear inside expressions and therefore mark
 *        the start of a statement when we parse sugar such as `if v < 10`.
 */
static bool token_is_statement_leader(const token_t *tok) {
    if (!tok || tok->kind != TOKEN_IDENTIFIER) {
        return false;
    }
    static const char *leaders[] = {
        "return",
        "break",
        "continue",
        "goto",
        "switch",
        "for",
        "while",
        "do",
        "else",
    };
    for (size_t i = 0; i < sizeof(leaders) / sizeof(leaders[0]); ++i) {
        if (token_is_identifier(tok, leaders[i])) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Mirror `rewrite_condition` but for increment expressions.
 */
static char *rewrite_increment(const char *expr, const char *loop_ident) {
    if (!expr) {
        return NULL;
    }
    if (!loop_ident || !*loop_ident) {
        return dup_string(expr);
    }
    const char *trim = expr;
    while (*trim && is_space(*trim)) {
        trim++;
    }
    if (*trim == '\0') {
        return dup_string(expr);
    }
    if (has_prefix(trim, "++") || has_prefix(trim, "--")) {
        size_t needed = 2 + strlen(loop_ident) + 1;
        char *out = malloc(needed);
        if (!out) {
            return NULL;
        }
        snprintf(out, needed, "%.2s%s", trim, loop_ident);
        return out;
    }
    if (has_prefix(trim, "+=") || has_prefix(trim, "-=") || has_prefix(trim, "*=") ||
        has_prefix(trim, "/=") || has_prefix(trim, "%=") ) {
        size_t needed = strlen(loop_ident) + 1 + strlen(trim) + 1;
        char *out = malloc(needed);
        if (!out) {
            return NULL;
        }
        snprintf(out, needed, "%s %s", loop_ident, trim);
        return out;
    }
    if (*trim == '.') {
        size_t needed = strlen(loop_ident) + strlen(trim) + 1;
        char *out = malloc(needed);
        if (!out) {
            return NULL;
        }
        snprintf(out, needed, "%s%s", loop_ident, trim);
        return out;
    }
    return dup_string(expr);
}

/**
 * @brief Parse an `@import` directive out of the token stream.
 */
bool parser_parse_import(const char *source,
                         const token_buffer_t *tokens,
                         size_t start_index,
                         import_directive_t *out,
                         diagnostic_t *diag) {
    if (!tokens || start_index + 1 >= tokens->count) {
        return false;
    }
    const token_t *at = &tokens->items[start_index];
    const token_t *kw = &tokens->items[start_index + 1];
    if (at->kind != TOKEN_AT || !token_is_identifier(kw, "import")) {
        return false;
    }
    size_t path_start = SIZE_MAX;
    size_t path_end = 0;
    size_t idx = start_index + 2;
    size_t end_offset = kw->offset + kw->length;
    while (idx < tokens->count) {
        const token_t *tok = &tokens->items[idx];
        if (tok->kind == TOKEN_NEWLINE) {
            end_offset = tok->offset + tok->length;
            idx++;
            break;
        }
        if (tok->kind == TOKEN_EOF) {
            end_offset = tok->offset;
            break;
        }
        if (path_start == SIZE_MAX) {
            path_start = tok->offset;
        }
        path_end = tok->offset + tok->length;
        end_offset = path_end;
        idx++;
    }
    if (path_start == SIZE_MAX || path_end <= path_start) {
        diagnostic_set(diag, at->line, "@import is missing a target");
        return false;
    }
    char *raw = copy_range(source, path_start, path_end);
    if (!raw) {
        diagnostic_set(diag, at->line, "out of memory while parsing @import");
        return false;
    }
    char *sanitized = copy_without_spaces(raw);
    free(raw);
    if (!sanitized) {
        diagnostic_set(diag, at->line, "out of memory while parsing @import");
        return false;
    }
    import_kind_t kind;
    const char *payload = NULL;
    if (strncmp(sanitized, "c/", 2) == 0) {
        kind = IMPORT_KIND_C;
        payload = sanitized + 2;
    } else if (strncmp(sanitized, "c++/", 4) == 0) {
        kind = IMPORT_KIND_CPP;
        payload = sanitized + 4;
    } else if (strncmp(sanitized, "godmode", 7) == 0 && sanitized[7] == '\0') {
        kind = IMPORT_KIND_GODMODE;
        payload = "";
    } else if (strncmp(sanitized, "std/string", 10) == 0 && sanitized[10] == '\0') {
        kind = IMPORT_KIND_STD_STRING;
        payload = "string";
    } else if (strncmp(sanitized, "std/math", 8) == 0 && sanitized[8] == '\0') {
        kind = IMPORT_KIND_STD_MATH;
        payload = "math";
    } else if (strncmp(sanitized, "std/stdlib", 10) == 0 && sanitized[10] == '\0') {
        kind = IMPORT_KIND_STD_STDLIB;
        payload = "stdlib";
    } else if (strncmp(sanitized, "std/time", 8) == 0 && sanitized[8] == '\0') {
        kind = IMPORT_KIND_STD_TIME;
        payload = "time";
    } else if (strncmp(sanitized, "std/assert", 10) == 0 && sanitized[10] == '\0') {
        kind = IMPORT_KIND_STD_ASSERT;
        payload = "assert";
    } else if (strncmp(sanitized, "std/errno", 9) == 0 && sanitized[9] == '\0') {
        kind = IMPORT_KIND_STD_ERRNO;
        payload = "errno";
    } else if (strncmp(sanitized, "std/file", 8) == 0 && sanitized[8] == '\0') {
        kind = IMPORT_KIND_STD_FILE;
        payload = "file";
    } else if (strncmp(sanitized, "file/core", 9) == 0 && sanitized[9] == '\0') {
        kind = IMPORT_KIND_FILE_CORE;
        payload = "file/core";
    } else if (strncmp(sanitized, "network/core", 12) == 0 && sanitized[12] == '\0') {
        kind = IMPORT_KIND_NETWORK_CORE;
        payload = "network/core";
    } else if (strncmp(sanitized, "std/", 4) == 0) {
        kind = IMPORT_KIND_STD;
        payload = sanitized + 4;
    } else if (strncmp(sanitized, "threads/core", 12) == 0 && sanitized[12] == '\0') {
        kind = IMPORT_KIND_THREADS_CORE;
        payload = "threads/core";
    } else if ((strncmp(sanitized, "process/core", 12) == 0 && sanitized[12] == '\0') ||
               (strcmp(sanitized, "process") == 0)) {
        kind = IMPORT_KIND_PROCESS_CORE;
        payload = "process/core";
    } else if (strncmp(sanitized, "matrix/core", 11) == 0 && sanitized[11] == '\0') {
        kind = IMPORT_KIND_MATRIX_CORE;
        payload = "matrix/core";
    } else {
        diagnostic_set(diag, at->line, "unsupported import prefix in '%s'", sanitized);
        free(sanitized);
        return false;
    }
    if (!*payload && kind != IMPORT_KIND_GODMODE) {
        diagnostic_set(diag, at->line, "@import target is empty");
        free(sanitized);
        return false;
    }
    char *target = dup_string(payload);
    free(sanitized);
    if (!target) {
        diagnostic_set(diag, at->line, "out of memory while storing import target");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->target = target;
    out->start_offset = at->offset;
    out->end_offset = end_offset;
    out->next_index = idx;
    return true;
}

/**
 * @brief Release heap memory owned by the parsed directive.
 */
void parser_free_import(import_directive_t *dir) {
    if (!dir) {
        return;
    }
    free(dir->target);
    dir->target = NULL;
}

/**
 * @brief Centralized cleanup for `for_header_t` strings.
 */
static void free_for_header_parts(for_header_t *header) {
    if (!header) {
        return;
    }
    free(header->initializer);
    free(header->condition);
    free(header->increment);
    free(header->trailing_ws);
    header->initializer = NULL;
    header->condition = NULL;
    header->increment = NULL;
    header->trailing_ws = NULL;
}

/**
 * @brief Parse the custom `for` header syntax.
 */
bool parser_parse_for_header(const char *source,
                             const token_buffer_t *tokens,
                             size_t start_index,
                             for_header_t *out,
                             diagnostic_t *diag) {
    if (!tokens || start_index >= tokens->count) {
        return false;
    }
    const token_t *kw = &tokens->items[start_index];
    if (!token_is_identifier(kw, "for")) {
        return false;
    }
    size_t idx = start_index + 1;
    while (idx < tokens->count && tokens->items[idx].kind == TOKEN_NEWLINE) {
        idx++;
    }
    if (idx >= tokens->count) {
        diagnostic_set(diag, kw->line, "unexpected end after 'for'");
        return false;
    }
    const token_t *maybe_paren = &tokens->items[idx];
    if (token_is_symbol(maybe_paren, '(')) {
        return false;
    }
    size_t segment_start = SIZE_MAX;
    size_t segment_end = 0;
    char *segments[3] = {0};
    size_t segment_count = 0;
    size_t trimmed_end = 0;
    size_t last_trimmed_end = kw->offset + kw->length;
    size_t block_offset = 0;
    size_t brace_index = 0;
    for (;;) {
        if (idx >= tokens->count) {
            diagnostic_set(diag, kw->line, "unterminated for header");
            goto fail;
        }
        const token_t *tok = &tokens->items[idx];
        if (tok->kind == TOKEN_NEWLINE) {
            idx++;
            continue;
        }
        if (token_is_symbol(tok, ';')) {
            if (segment_start == SIZE_MAX) {
                diagnostic_set(diag, kw->line, "empty for segment");
                goto fail;
            }
            if (segment_count >= 3) {
                diagnostic_set(diag, kw->line, "too many segments in for header");
                goto fail;
            }
            segments[segment_count] = copy_trimmed(source, segment_start, segment_end, &trimmed_end);
            if (!segments[segment_count]) {
                diagnostic_set(diag, kw->line, "out of memory while parsing for header");
                goto fail;
            }
            last_trimmed_end = trimmed_end;
            segment_count++;
            segment_start = SIZE_MAX;
            segment_end = 0;
            idx++;
            continue;
        }
        if (token_is_symbol(tok, '{')) {
            if (segment_start != SIZE_MAX) {
                if (segment_count >= 3) {
                    diagnostic_set(diag, kw->line, "too many segments in for header");
                    goto fail;
                }
                segments[segment_count] = copy_trimmed(source, segment_start, segment_end, &trimmed_end);
                if (!segments[segment_count]) {
                    diagnostic_set(diag, kw->line, "out of memory while parsing for header");
                    goto fail;
                }
                last_trimmed_end = trimmed_end;
                segment_count++;
            }
            block_offset = tok->offset;
            brace_index = idx;
            break;
        }
        if (segment_start == SIZE_MAX) {
            segment_start = tok->offset;
        }
        segment_end = tok->offset + tok->length;
        idx++;
    }
    if (segment_count < 2 || segment_count > 3) {
        diagnostic_set(diag, kw->line, "for header requires two or three segments");
        goto fail;
    }
    memset(out, 0, sizeof(*out));
    out->has_initializer = (segment_count == 3);
    out->initializer = out->has_initializer ? segments[0] : NULL;
    if (!out->has_initializer) {
        free(segments[0]);
        segments[0] = NULL;
    }
    out->condition = segments[out->has_initializer ? 1 : 0];
    segments[out->has_initializer ? 1 : 0] = NULL;
    out->increment = segments[out->has_initializer ? 2 : 1];
    segments[out->has_initializer ? 2 : 1] = NULL;
    bool initializer_has_type = false;
    if (out->has_initializer &&
        detect_loop_identifier(out->initializer,
                               out->loop_identifier,
                               sizeof(out->loop_identifier),
                               &initializer_has_type)) {
        out->has_loop_identifier = true;
        if (!initializer_has_type) {
            const char *trimmed = out->initializer;
            while (*trimmed && is_space(*trimmed)) {
                trimmed++;
            }
            size_t trimmed_len = strlen(trimmed);
            size_t needed = strlen("auto ") + trimmed_len + 1;
            char *auto_init = malloc(needed);
            if (!auto_init) {
                diagnostic_set(diag, kw->line, "out of memory while inserting implicit loop type");
                goto fail;
            }
            snprintf(auto_init, needed, "auto %s", trimmed);
            free(out->initializer);
            out->initializer = auto_init;
        }
        char *cond = rewrite_condition(out->condition, out->loop_identifier);
        if (!cond) {
            diagnostic_set(diag, kw->line, "out of memory while rewriting for condition");
            goto fail;
        }
        free(out->condition);
        out->condition = cond;
        char *inc = rewrite_increment(out->increment, out->loop_identifier);
        if (!inc) {
            diagnostic_set(diag, kw->line, "out of memory while rewriting for increment");
            goto fail;
        }
        free(out->increment);
        out->increment = inc;
    }
    out->start_offset = kw->offset;
    out->block_offset = block_offset;
    out->next_index = brace_index;
    out->trailing_ws = copy_range(source, last_trimmed_end, block_offset);
    if (!out->trailing_ws) {
        diagnostic_set(diag, kw->line, "out of memory while preserving whitespace");
        goto fail;
    }
    if (out->trailing_ws[0] == '\0') {
        free(out->trailing_ws);
        out->trailing_ws = dup_string(" ");
        if (!out->trailing_ws) {
            diagnostic_set(diag, kw->line, "out of memory while keeping spacing");
            goto fail;
        }
    }
    return true;

fail:
    for (size_t i = 0; i < 3; ++i) {
        free(segments[i]);
    }
    free_for_header_parts(out);
    memset(out, 0, sizeof(*out));
    return false;
}

/**
 * @brief Public cleanup helper exposed to the transpiler.
 */
void parser_free_for_header(for_header_t *header) {
    free_for_header_parts(header);
}

/**
 * @brief Parse a sugar `if` statement without parentheses.
 */
static bool parse_condition_header(const char *source,
                                   const token_buffer_t *tokens,
                                   size_t start_index,
                                   const char *keyword,
                                   condition_header_t *out,
                                   diagnostic_t *diag) {
    if (!tokens || start_index >= tokens->count) {
        return false;
    }
    const token_t *kw = &tokens->items[start_index];
    if (!token_is_identifier(kw, keyword)) {
        return false;
    }
    size_t cond_start = kw->offset + kw->length;
    while (source[cond_start] != '\0' && is_space(source[cond_start])) {
        cond_start++;
    }
    if (source[cond_start] == '\0') {
        diagnostic_set(diag, kw->line, "%s is missing a condition", keyword);
        return false;
    }
    size_t idx = start_index + 1;
    bool condition_started = false;
    size_t boundary_offset = SIZE_MAX;
    size_t last_condition_index = idx;
    while (idx < tokens->count) {
        const token_t *tok = &tokens->items[idx];
        if (tok->kind == TOKEN_EOF) {
            break;
        }
        if (!condition_started && tok->kind == TOKEN_NEWLINE) {
            idx++;
            continue;
        }
        if (tok->kind == TOKEN_NEWLINE) {
            boundary_offset = tok->offset;
            break;
        }
        if (token_is_symbol(tok, '{')) {
            boundary_offset = tok->offset;
            break;
        }
        if (token_is_statement_leader(tok)) {
            boundary_offset = tok->offset;
            break;
        }
        condition_started = true;
        last_condition_index = idx;
        idx++;
    }
    if (!condition_started) {
        diagnostic_set(diag, kw->line, "%s condition is empty", keyword);
        return false;
    }
    if (boundary_offset == SIZE_MAX) {
        const token_t *last_tok = &tokens->items[last_condition_index];
        boundary_offset = last_tok->offset + last_tok->length;
        idx = last_condition_index + 1;
    }
    size_t trimmed_end = 0;
    char *cond = copy_trimmed(source, cond_start, boundary_offset, &trimmed_end);
    if (!cond) {
        diagnostic_set(diag, kw->line, "out of memory while parsing %s", keyword);
        return false;
    }
    if (cond[0] == '\0') {
        free(cond);
        diagnostic_set(diag, kw->line, "%s condition is empty", keyword);
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->condition = cond;
    out->start_offset = kw->offset;
    out->condition_end_offset = trimmed_end;
    out->body_offset = boundary_offset;
    out->next_index = idx;
    return true;
}

static void free_condition_header(condition_header_t *header) {
    if (!header) {
        return;
    }
    free(header->condition);
    header->condition = NULL;
}

bool parser_parse_if_header(const char *source,
                            const token_buffer_t *tokens,
                            size_t start_index,
                            if_header_t *out,
                            diagnostic_t *diag) {
    return parse_condition_header(source, tokens, start_index, "if", out, diag);
}

void parser_free_if_header(if_header_t *header) {
    free_condition_header(header);
}

bool parser_parse_while_header(const char *source,
                               const token_buffer_t *tokens,
                               size_t start_index,
                               while_header_t *out,
                               diagnostic_t *diag) {
    return parse_condition_header(source, tokens, start_index, "while", out, diag);
}

void parser_free_while_header(while_header_t *header) {
    free_condition_header(header);
}
