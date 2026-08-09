#define _GNU_SOURCE
#include "transpiler.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
extern char *realpath(const char *path, char *resolved_path);
#endif

#include "lexer.h"
#include "parser.h"
#include "sema.h"

/**
 * @file transpiler.c
 * @brief Full-source transpiler that bridges concept-style `.margo` syntax and
 *        the generated C fed into clang. Every routine carries detailed Doxygen
 *        comments so that the document effectively serves as a design memo.
 *
 * Self-hosting additions (beyond the original prototype):
 *  - RAII scope-exit free injection using the ownership table produced by
 *    the semantic analysis pass.
 *  - Return-path free injection so owned allocations are freed even when a
 *    function returns early through any path.
 *  - `null` keyword lowering to the C `NULL` macro.
 *  - `@import godmode` expanding to a full set of standard C headers.
 *  - `@import c++/...` stub that preserves the directive as a comment so
 *    the C++ binding milestone can be implemented without breaking existing
 *    source files.
 *  - Extended `@import std/...` module mapping (string, math, stdlib, time,
 *    assert, errno).
 */

/**
 * @brief Slurp an entire file into memory.
 */
static bool read_file(const char *path, char **buffer, size_t *size, diagnostic_t *diag) {
    FILE *in = fopen(path, "rb");
    if (!in) {
        diagnostic_set(diag, 0, "failed to open %s: %s", path, strerror(errno));
        return false;
    }
    if (fseek(in, 0, SEEK_END) != 0) {
        diagnostic_set(diag, 0, "failed to seek %s", path);
        fclose(in);
        return false;
    }
    long len = ftell(in);
    if (len < 0) {
        diagnostic_set(diag, 0, "failed to measure %s", path);
        fclose(in);
        return false;
    }
    if (fseek(in, 0, SEEK_SET) != 0) {
        diagnostic_set(diag, 0, "failed to rewind %s", path);
        fclose(in);
        return false;
    }
    size_t size_t_len = (size_t)len;
    char *data = malloc(size_t_len + 1);
    if (!data) {
        diagnostic_set(diag, 0, "out of memory reading %s", path);
        fclose(in);
        return false;
    }
    size_t read = fread(data, 1, size_t_len, in);
    fclose(in);
    if (read != size_t_len) {
        free(data);
        diagnostic_set(diag, 0, "failed to read full file %s", path);
        return false;
    }
    data[size_t_len] = '\0';
    *buffer = data;
    *size = size_t_len;
    return true;
}

/**
 * @brief Copy a contiguous slice of the source file into the output buffer.
 */
static bool transpiler_copy_range(FILE *out, const char *src, size_t start, size_t end) {
    if (end <= start) {
        return true;
    }
    size_t len = end - start;
    return fwrite(src + start, 1, len, out) == len;
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char *path;
    char *content;
    size_t length;
} flattened_file_t;

typedef struct {
    flattened_file_t *items;
    size_t count;
    size_t capacity;
} flattened_file_list_t;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} path_list_t;

static char *tp_strdup(const char *src) {
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *dup = malloc(len + 1);
    if (!dup) {
        return NULL;
    }
    memcpy(dup, src, len + 1);
    return dup;
}

static char *tp_strndup(const char *src, size_t len) {
    if (!src) {
        return NULL;
    }
    char *dup = malloc(len + 1);
    if (!dup) {
        return NULL;
    }
    memcpy(dup, src, len);
    dup[len] = '\0';
    return dup;
}

static void flattened_file_list_free(flattened_file_list_t *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].path);
        free(list->items[i].content);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool flattened_file_list_append(flattened_file_list_t *list,
                                       const char *path,
                                       char *content,
                                       size_t length) {
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 8;
        flattened_file_t *new_items = realloc(list->items, new_cap * sizeof(*new_items));
        if (!new_items) {
            free(content);
            return false;
        }
        list->items = new_items;
        list->capacity = new_cap;
    }
    char *path_copy = tp_strdup(path);
    if (!path_copy) {
        free(content);
        return false;
    }
    list->items[list->count].path = path_copy;
    list->items[list->count].content = content;
    list->items[list->count].length = length;
    list->count++;
    return true;
}

static bool path_list_contains(const path_list_t *list, const char *path) {
    if (!list || !path) {
        return false;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], path) == 0) {
            return true;
        }
    }
    return false;
}

static bool path_list_push(path_list_t *list, const char *path) {
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 8;
        char **new_items = realloc(list->items, new_cap * sizeof(*new_items));
        if (!new_items) {
            return false;
        }
        list->items = new_items;
        list->capacity = new_cap;
    }
    char *copy = tp_strdup(path);
    if (!copy) {
        return false;
    }
    list->items[list->count++] = copy;
    return true;
}

static void path_list_pop(path_list_t *list) {
    if (!list || list->count == 0) {
        return;
    }
    free(list->items[list->count - 1]);
    list->items[list->count - 1] = NULL;
    list->count--;
}

static void path_list_free(path_list_t *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

typedef struct {
    char **names;
    size_t count;
    size_t capacity;
} struct_tag_list_t;

static void struct_tag_list_free(struct_tag_list_t *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        free(list->names[i]);
    }
    free(list->names);
    list->names = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool struct_tag_list_contains(const struct_tag_list_t *list, const char *text, size_t len) {
    if (!list || !text) {
        return false;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (strlen(list->names[i]) == len && strncmp(list->names[i], text, len) == 0) {
            return true;
        }
    }
    return false;
}

static bool struct_tag_list_append(struct_tag_list_t *list, const char *text, size_t len) {
    if (!list || !text || len == 0) {
        return true;
    }
    if (struct_tag_list_contains(list, text, len)) {
        return true;
    }
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 8;
        char **new_items = realloc(list->names, new_cap * sizeof(char *));
        if (!new_items) {
            return false;
        }
        list->names = new_items;
        list->capacity = new_cap;
    }
    char *copy = tp_strndup(text, len);
    if (!copy) {
        return false;
    }
    list->names[list->count++] = copy;
    return true;
}

static size_t skip_newlines(const token_buffer_t *tokens, size_t index) {
    size_t i = index;
    while (i < tokens->count && tokens->items[i].kind == TOKEN_NEWLINE) {
        i++;
    }
    return i;
}

static bool collect_struct_tags(const token_buffer_t *tokens, struct_tag_list_t *tags) {
    if (!tokens || !tags) {
        return true;
    }
    for (size_t i = 0; i < tokens->count; ++i) {
        const token_t *tok = &tokens->items[i];
        if (tok->kind != TOKEN_IDENTIFIER) {
            continue;
        }
        if (!token_is_identifier(tok, "struct")) {
            continue;
        }
        size_t name_idx = skip_newlines(tokens, i + 1);
        if (name_idx >= tokens->count) {
            continue;
        }
        const token_t *name_tok = &tokens->items[name_idx];
        if (name_tok->kind != TOKEN_IDENTIFIER) {
            continue;
        }
        if (!struct_tag_list_append(tags, name_tok->lexeme, name_tok->length)) {
            return false;
        }
    }
    return true;
}

static bool append_bytes(char **buf, size_t *cap, size_t *used, const char *data, size_t len) {
    if (!buf || !cap || !used || !data || len == 0) {
        if (len == 0) {
            return true;
        }
    }
    size_t needed = *used + len + 1;
    if (needed > *cap) {
        size_t new_cap = *cap ? *cap : 64;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf) {
            return false;
        }
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *used, data, len);
    *used += len;
    return true;
}

static bool rewrite_weird_struct_args(char **source,
                                      size_t *length,
                                      const token_buffer_t *tokens,
                                      diagnostic_t *diag,
                                      bool *changed_out) {
    if (!source || !length || !tokens) {
        return true;
    }
    if (changed_out) {
        *changed_out = false;
    }
    struct_tag_list_t tags = {0};
    if (!collect_struct_tags(tokens, &tags)) {
        struct_tag_list_free(&tags);
        diagnostic_set(diag, 0, "out of memory while tracking struct tags");
        return false;
    }
    if (tags.count == 0) {
        struct_tag_list_free(&tags);
        return true;
    }
    size_t cap = *length + 1;
    char *out = malloc(cap);
    if (!out) {
        struct_tag_list_free(&tags);
        diagnostic_set(diag, 0, "out of memory while normalizing weird() arguments");
        return false;
    }
    size_t written = 0;
    size_t last_emit = 0;
    bool changed = false;
    for (size_t i = 0; i < tokens->count; ++i) {
        const token_t *tok = &tokens->items[i];
        if (tok->kind != TOKEN_IDENTIFIER || !token_is_identifier(tok, "weird")) {
            continue;
        }
        size_t open_idx = skip_newlines(tokens, i + 1);
        if (open_idx >= tokens->count || !token_is_symbol(&tokens->items[open_idx], '(')) {
            continue;
        }
        size_t arg_idx = skip_newlines(tokens, open_idx + 1);
        if (arg_idx >= tokens->count) {
            continue;
        }
        const token_t *arg_tok = &tokens->items[arg_idx];
        if (arg_tok->kind != TOKEN_IDENTIFIER) {
            continue;
        }
        if (token_is_identifier(arg_tok, "struct")) {
            continue;
        }
        if (!struct_tag_list_contains(&tags, arg_tok->lexeme, arg_tok->length)) {
            continue;
        }
        size_t prefix_len = arg_tok->offset - last_emit;
        if (!append_bytes(&out, &cap, &written, *source + last_emit, prefix_len) ||
            !append_bytes(&out, &cap, &written, "struct ", strlen("struct ")) ||
            !append_bytes(&out, &cap, &written, arg_tok->lexeme, arg_tok->length)) {
            free(out);
            struct_tag_list_free(&tags);
            diagnostic_set(diag, 0, "out of memory while normalizing weird() arguments");
            return false;
        }
        last_emit = arg_tok->offset + arg_tok->length;
        changed = true;
    }
    if (!changed) {
        struct_tag_list_free(&tags);
        free(out);
        return true;
    }
    if (last_emit < *length) {
        if (!append_bytes(&out, &cap, &written, *source + last_emit, *length - last_emit)) {
            free(out);
            struct_tag_list_free(&tags);
            diagnostic_set(diag, 0, "out of memory while finalizing weird() normalization");
            return false;
        }
    }
    out[written] = '\0';
    free(*source);
    *source = out;
    *length = written;
    struct_tag_list_free(&tags);
    if (changed_out) {
        *changed_out = true;
    }
    return true;
}

static const char *find_last_separator(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (!slash) {
        return bslash;
    }
    if (!bslash) {
        return slash;
    }
    return (slash > bslash) ? slash : bslash;
}

static void parent_directory(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz == 0) {
        return;
    }
    const char *sep = find_last_separator(path);
    if (!sep) {
        snprintf(out, out_sz, ".");
        return;
    }
    size_t len = (size_t)(sep - path);
#ifdef _WIN32
    if (len == 0) {
        len = 1;
    } else if (len == 2 && path[1] == ':') {
        len = 3;
    }
#else
    if (len == 0) {
        len = 1;
    }
#endif
    snprintf(out, out_sz, "%.*s", (int)len, path);
}

static bool is_absolute_path(const char *path) {
    if (!path || !*path) {
        return false;
    }
#ifdef _WIN32
    if ((strlen(path) >= 2 && path[1] == ':') &&
        ((strlen(path) >= 3 && (path[2] == '/' || path[2] == '\\')))) {
        return true;
    }
    return path[0] == '/' || path[0] == '\\';
#else
    return path[0] == '/';
#endif
}

static bool canonicalize_path(const char *input, char *resolved, size_t resolved_sz, diagnostic_t *diag) {
    if (!input || !resolved || resolved_sz == 0) {
        diagnostic_set(diag, 0, "invalid path resolution request");
        return false;
    }
#ifdef _WIN32
    if (!_fullpath(resolved, input, resolved_sz)) {
        diagnostic_set(diag, 0, "failed to resolve %s: %s", input, strerror(errno));
        return false;
    }
#else
    if (!realpath(input, resolved)) {
        diagnostic_set(diag, 0, "failed to resolve %s: %s", input, strerror(errno));
        return false;
    }
#endif
    return true;
}

static bool resolve_local_import_path(const char *parent_path,
                                      const char *target,
                                      char *resolved,
                                      size_t resolved_sz,
                                      diagnostic_t *diag) {
    if (!target || !*target) {
        diagnostic_set(diag, 0, "@import target is empty");
        return false;
    }
    char candidate[PATH_MAX];
    if (is_absolute_path(target)) {
        if (snprintf(candidate, sizeof(candidate), "%s", target) >= (int)sizeof(candidate)) {
            diagnostic_set(diag, 0, "path too long: %s", target);
            return false;
        }
    } else {
        char parent_dir[PATH_MAX];
        parent_directory(parent_path, parent_dir, sizeof(parent_dir));
        if (snprintf(candidate, sizeof(candidate), "%s/%s", parent_dir, target) >= (int)sizeof(candidate)) {
            diagnostic_set(diag, 0, "path too long after joining %s and %s", parent_dir, target);
            return false;
        }
    }
    return canonicalize_path(candidate, resolved, resolved_sz, diag);
}

static bool flatten_collect_file(const char *path,
                                 flattened_file_list_t *outputs,
                                 path_list_t *stack,
                                 path_list_t *visited,
                                 diagnostic_t *diag);

static bool flatten_entry_file(const char *entry_path,
                               char **flat_source,
                               size_t *flat_length,
                               diagnostic_t *diag) {
    flattened_file_list_t outputs = {0};
    path_list_t stack = {0};
    path_list_t visited = {0};
    char resolved[PATH_MAX];
    bool ok = canonicalize_path(entry_path, resolved, sizeof(resolved), diag);
    if (ok) {
        ok = flatten_collect_file(resolved, &outputs, &stack, &visited, diag);
    }
    if (!ok) {
        flattened_file_list_free(&outputs);
        path_list_free(&stack);
        path_list_free(&visited);
        return false;
    }
    FILE *out = open_memstream(flat_source, flat_length);
    if (!out) {
        diagnostic_set(diag, 0, "failed to allocate flattened buffer");
        flattened_file_list_free(&outputs);
        path_list_free(&stack);
        path_list_free(&visited);
        return false;
    }
    fputs("// AUTO-GENERATED: flattened Margo translation unit\n", out);
    for (size_t i = 0; i < outputs.count; ++i) {
        fprintf(out, "\n// ---- %s ----\n", outputs.items[i].path);
        if (outputs.items[i].length > 0) {
            fwrite(outputs.items[i].content, 1, outputs.items[i].length, out);
            if (outputs.items[i].content[outputs.items[i].length - 1] != '\n') {
                fputc('\n', out);
            }
        }
    }
    fclose(out);
    flattened_file_list_free(&outputs);
    path_list_free(&stack);
    path_list_free(&visited);
    return true;
}

static bool flatten_collect_file(const char *path,
                                 flattened_file_list_t *outputs,
                                 path_list_t *stack,
                                 path_list_t *visited,
                                 diagnostic_t *diag) {
    if (path_list_contains(visited, path)) {
        return true;
    }
    if (path_list_contains(stack, path)) {
        diagnostic_set(diag, 0, "detected recursive import while expanding %s", path);
        return false;
    }
    if (!path_list_push(stack, path)) {
        diagnostic_set(diag, 0, "out of memory tracking import stack");
        return false;
    }
    char *source = NULL;
    size_t source_len = 0;
    if (!read_file(path, &source, &source_len, diag)) {
        path_list_pop(stack);
        return false;
    }
    token_buffer_t tokens;
    if (!lexer_tokenize(source, source_len, &tokens, diag)) {
        free(source);
        path_list_pop(stack);
        return false;
    }
    char *kept_buf = NULL;
    size_t kept_len = 0;
    FILE *kept = open_memstream(&kept_buf, &kept_len);
    if (!kept) {
        diagnostic_set(diag, 0, "failed to create flattened buffer");
        lexer_free(&tokens);
        free(source);
        path_list_pop(stack);
        return false;
    }
    size_t last_emit = 0;
    size_t i = 0;
    while (i < tokens.count) {
        import_directive_t dir;
        if (parser_parse_import(source, &tokens, i, &dir, diag)) {
            size_t next_index = dir.next_index;
            if (dir.kind == IMPORT_KIND_LOCAL) {
                if (!transpiler_copy_range(kept, source, last_emit, dir.start_offset)) {
                    parser_free_import(&dir);
                    fclose(kept);
                    free(kept_buf);
                    lexer_free(&tokens);
                    free(source);
                    path_list_pop(stack);
                    diagnostic_set(diag, 0, "failed to copy local import prelude");
                    return false;
                }
                last_emit = dir.end_offset;
                char child_path[PATH_MAX];
                if (!resolve_local_import_path(path, dir.target, child_path, sizeof(child_path), diag)) {
                    parser_free_import(&dir);
                    fclose(kept);
                    free(kept_buf);
                    lexer_free(&tokens);
                    free(source);
                    path_list_pop(stack);
                    return false;
                }
                bool ok = flatten_collect_file(child_path, outputs, stack, visited, diag);
                parser_free_import(&dir);
                if (!ok) {
                    fclose(kept);
                    free(kept_buf);
                    lexer_free(&tokens);
                    free(source);
                    path_list_pop(stack);
                    return false;
                }
                if (next_index > i) {
                    i = next_index;
                } else {
                    i++;
                }
                continue;
            }
            parser_free_import(&dir);
            if (next_index > i) {
                i = next_index;
            } else {
                i++;
            }
            continue;
        }
        if (diag && diag->has_error) {
            fclose(kept);
            free(kept_buf);
            lexer_free(&tokens);
            free(source);
            path_list_pop(stack);
            return false;
        }
        i++;
    }
    bool copy_ok = transpiler_copy_range(kept, source, last_emit, source_len);
    fclose(kept);
    lexer_free(&tokens);
    free(source);
    if (!copy_ok) {
        free(kept_buf);
        path_list_pop(stack);
        diagnostic_set(diag, 0, "failed to finalize flattened buffer");
        return false;
    }
    if (!path_list_push(visited, path)) {
        free(kept_buf);
        path_list_pop(stack);
        diagnostic_set(diag, 0, "out of memory recording visited imports");
        return false;
    }
    path_list_pop(stack);
    bool appended = flattened_file_list_append(outputs, path, kept_buf, kept_len);
    if (!appended) {
        diagnostic_set(diag, 0, "out of memory storing flattened file");
        return false;
    }
    return true;
}

static char *duplicate_range(const char *src, size_t start, size_t end) {
    if (end < start) {
        end = start;
    }
    size_t len = end - start;
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    if (len) {
        memcpy(copy, src + start, len);
    }
    copy[len] = '\0';
    return copy;
}

typedef struct {
    int depth_before;
    bool unwrap;
    bool pending_open;
} style_block_state_t;

typedef struct {
    style_block_state_t *items;
    size_t count;
    size_t capacity;
} style_block_stack_t;

static void style_stack_free(style_block_stack_t *stack) {
    if (!stack) {
        return;
    }
    free(stack->items);
    stack->items = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static bool style_stack_push(style_block_stack_t *stack, int depth_before, bool unwrap) {
    if (!stack) {
        return false;
    }
    if (stack->count == stack->capacity) {
        size_t new_capacity = stack->capacity ? stack->capacity * 2 : 8;
        style_block_state_t *new_items =
            realloc(stack->items, new_capacity * sizeof(style_block_state_t));
        if (!new_items) {
            return false;
        }
        stack->items = new_items;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count++] = (style_block_state_t){
        .depth_before = depth_before,
        .unwrap = unwrap,
        .pending_open = true,
    };
    return true;
}

static style_block_state_t *style_stack_top(style_block_stack_t *stack) {
    if (!stack || stack->count == 0) {
        return NULL;
    }
    return &stack->items[stack->count - 1];
}

static bool style_handle_open_brace(style_block_stack_t *stack,
                                    const char *source,
                                    FILE *out,
                                    const token_t *tok,
                                    size_t *last_emit,
                                    diagnostic_t *diag) {
    style_block_state_t *top = style_stack_top(stack);
    if (!top || !top->pending_open) {
        return true;
    }
    top->pending_open = false;
    if (!top->unwrap) {
        return true;
    }
    if (!transpiler_copy_range(out, source, *last_emit, tok->offset)) {
        diagnostic_set(diag, tok->line, "failed to preserve @style prefix before '{'");
        return false;
    }
    *last_emit = tok->offset + tok->length;
    return true;
}

static bool style_handle_close_brace(style_block_stack_t *stack,
                                     const char *source,
                                     FILE *out,
                                     const token_t *tok,
                                     size_t *last_emit,
                                     int brace_depth,
                                     diagnostic_t *diag) {
    style_block_state_t *top = style_stack_top(stack);
    if (!top) {
        return true;
    }
    if (brace_depth != top->depth_before + 1) {
        return true;
    }
    if (top->unwrap) {
        if (!transpiler_copy_range(out, source, *last_emit, tok->offset)) {
            diagnostic_set(diag, tok->line, "failed to preserve @style prefix before '}'");
            return false;
        }
        *last_emit = tok->offset + tok->length;
    }
    stack->count--;
    return true;
}

/* =========================================================================
 * Blocked if (`if cond` newline body ... `else`/`endif`) tracking
 * ====================================================================== */

/**
 * @brief Stack of open blocked-if frames, each recording the brace depth of
 *        the current branch body (construct depth + 1).  Frames are keyed by
 *        depth so plain C `else` inside nested braces is left untouched.
 */
typedef struct {
    int *items;
    size_t count;
    size_t capacity;
} blocked_if_stack_t;

static void blocked_if_stack_free(blocked_if_stack_t *stack) {
    if (!stack) {
        return;
    }
    free(stack->items);
    stack->items = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static bool blocked_if_stack_push(blocked_if_stack_t *stack, int depth) {
    if (!stack) {
        return false;
    }
    if (stack->count == stack->capacity) {
        size_t new_capacity = stack->capacity ? stack->capacity * 2 : 8;
        int *new_items = realloc(stack->items, new_capacity * sizeof(int));
        if (!new_items) {
            return false;
        }
        stack->items = new_items;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count++] = depth;
    return true;
}

static bool blocked_if_stack_matches(const blocked_if_stack_t *stack, int depth) {
    return stack && stack->count > 0 && stack->items[stack->count - 1] == depth;
}

/* =========================================================================
 * RAII scope-exit free injection
 * ====================================================================== */

/**
 * @brief One variable known to be alloc-owned at the given brace depth.
 */
typedef struct {
    char *name;        /**< Heap-allocated identifier string */
    int   scope_depth; /**< Brace depth at point of declaration */
} raii_live_var_t;

/**
 * @brief Runtime tracker used during the main transpile pass.
 *
 * Variables are pushed when an `alloc`/`alloc_and_init` assignment is
 * detected and popped (with free injection) when the enclosing scope's
 * closing brace is emitted.
 */
typedef struct {
    raii_live_var_t *items;
    size_t           count;
    size_t           capacity;
} raii_live_t;

static bool raii_live_push(raii_live_t *live, const char *name, int depth) {
    if (!live || !name) {
        return false;
    }
    if (live->count == live->capacity) {
        size_t new_cap = live->capacity ? live->capacity * 2 : 16;
        raii_live_var_t *new_items = realloc(live->items, new_cap * sizeof(raii_live_var_t));
        if (!new_items) {
            return false;
        }
        live->items    = new_items;
        live->capacity = new_cap;
    }
    size_t name_len = strlen(name);
    char *copy = malloc(name_len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, name, name_len + 1);
    live->items[live->count].name        = copy;
    live->items[live->count].scope_depth = depth;
    live->count++;
    return true;
}

/**
 * @brief Emit `free()` calls for every owned variable at exactly `depth`
 *        and remove those entries from the tracker.
 */
static bool raii_live_emit_scope_frees(raii_live_t *live, int depth, FILE *out) {
    if (!live) {
        return true;
    }
    bool ok = true;
    for (size_t i = live->count; i > 0; --i) {
        raii_live_var_t *v = &live->items[i - 1];
        if (v->scope_depth != depth) {
            continue;
        }
        if (fprintf(out, "margo_builtin_free_ptr(%s);\n", v->name) < 0) {
            ok = false;
        }
        free(v->name);
        /* Remove by shifting */
        memmove(&live->items[i - 1], &live->items[i],
                (live->count - i) * sizeof(raii_live_var_t));
        live->count--;
    }
    return ok;
}

/**
 * @brief Emit `free()` calls for all owned variables in the depth range
 *        [min_depth, max_depth] (inclusive).  Used before `return`.
 *
 * An optional `skip_name` prevents freeing the variable being returned
 * (ownership transfer).
 */
static bool raii_live_emit_return_frees(raii_live_t *live,
                                         int min_depth, int max_depth,
                                         const char *skip_name,
                                         FILE *out) {
    if (!live) {
        return true;
    }
    bool ok = true;
    for (size_t i = live->count; i > 0; --i) {
        raii_live_var_t *v = &live->items[i - 1];
        if (v->scope_depth < min_depth || v->scope_depth > max_depth) {
            continue;
        }
        if (skip_name && strcmp(v->name, skip_name) == 0) {
            continue;
        }
        if (fprintf(out, "margo_builtin_free_ptr(%s);\n", v->name) < 0) {
            ok = false;
        }
    }
    return ok;
}

static void raii_live_free(raii_live_t *live) {
    if (!live) {
        return;
    }
    for (size_t i = 0; i < live->count; ++i) {
        free(live->items[i].name);
    }
    free(live->items);
    live->items    = NULL;
    live->count    = 0;
    live->capacity = 0;
}

/* =========================================================================
 * defer stack (explicit scope-exit statements)
 * ====================================================================== */
typedef struct {
    char *expression; /**< Heap-allocated expression string */
    int   scope_depth; /**< Brace depth at point of defer */
} defer_entry_t;

typedef struct {
    defer_entry_t *items;
    size_t         count;
    size_t         capacity;
} defer_stack_t;

static bool defer_push(defer_stack_t *stack, const char *expression, int depth) {
    if (!stack || !expression) {
        return false;
    }
    (void)depth; /* used in debug builds */
    if (stack->count == stack->capacity) {
        size_t new_cap = stack->capacity ? stack->capacity * 2 : 16;
        defer_entry_t *new_items = realloc(stack->items, new_cap * sizeof(defer_entry_t));
        if (!new_items) {
            return false;
        }
        stack->items = new_items;
        stack->capacity = new_cap;
    }
    size_t expr_len = strlen(expression);
    char *copy = malloc(expr_len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, expression, expr_len + 1);
    stack->items[stack->count].expression = copy;
    stack->items[stack->count].scope_depth = depth;
    stack->count++;
    return true;
}

static bool defer_emit_scope(defer_stack_t *stack, int depth, FILE *out) {
    if (!stack) {
        return true;
    }
    bool ok = true;
    for (size_t i = stack->count; i > 0; --i) {
        defer_entry_t *e = &stack->items[i - 1];
        if (e->scope_depth != depth) {
            continue;
        }
        if (fprintf(out, "(%s);\n", e->expression) < 0) {
            ok = false;
        }
        free(e->expression);
        memmove(&stack->items[i - 1], &stack->items[i],
                (stack->count - i) * sizeof(defer_entry_t));
        stack->count--;
    }
    return ok;
}

static bool defer_emit_return(defer_stack_t *stack, int min_depth, int max_depth, FILE *out) {
    if (!stack) {
        return true;
    }
    bool ok = true;
    for (size_t i = stack->count; i > 0; --i) {
        defer_entry_t *e = &stack->items[i - 1];
        if (e->scope_depth < min_depth || e->scope_depth > max_depth) {
            continue;
        }
        if (fprintf(out, "(%s);\n", e->expression) < 0) {
            ok = false;
        }
        free(e->expression);
        memmove(&stack->items[i - 1], &stack->items[i],
                (stack->count - i) * sizeof(defer_entry_t));
        stack->count--;
    }
    return ok;
}

static void defer_free(defer_stack_t *stack) {
    if (!stack) {
        return;
    }
    for (size_t i = 0; i < stack->count; ++i) {
        free(stack->items[i].expression);
    }
    free(stack->items);
    stack->items = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

/**
 * @brief Check whether token at *index* is `IDENT = alloc(` and register
 *        the variable as owned if so.
 *
 * Advances nothing — this is a pure lookahead that does not move *index*.
 */
static bool token_is_owned_factory(const token_t *tok) {
    if (!tok || tok->kind != TOKEN_IDENTIFIER) {
        return false;
    }
    static const char *factories[] = {
        "alloc",
        "alloc_and_init",
        "owned_new",
        "owned_array",
        "matrix_fill",
        "matrix_identity",
        "matrix_mul",
        "matrix_transpose",
        "matrix_map",
    };
    for (size_t i = 0; i < sizeof(factories) / sizeof(factories[0]); ++i) {
        if (token_is_identifier(tok, factories[i])) {
            return true;
        }
    }
    return false;
}

static void try_register_alloc_ownership(const token_buffer_t *tokens,
                                         size_t index,
                                         int brace_depth,
                                         raii_live_t *live) {
    if (!tokens || !live) {
        return;
    }
    const token_t *tok = &tokens->items[index];
    if (tok->kind != TOKEN_IDENTIFIER) {
        return;
    }

    if (index > 0) {
        size_t p = index - 1;
        while (p > 0 && tokens->items[p].kind == TOKEN_NEWLINE) {
            p--;
        }
        if (tokens->items[p].kind == TOKEN_SYMBOL && tokens->items[p].length > 0) {
            char c = tokens->items[p].lexeme[tokens->items[p].length - 1];
            if (c == '.' || c == '>') {
                return; /* Do not register struct fields */
            }
        }
    }
    /* Build a local name buffer */
    char name[128] = {0};
    size_t name_len = tok->length < sizeof(name) - 1 ? tok->length : sizeof(name) - 1;
    memcpy(name, tok->lexeme, name_len);

    /* Skip forward to the next non-newline token */
    size_t j = index + 1;
    while (j < tokens->count && tokens->items[j].kind == TOKEN_NEWLINE) {
        j++;
    }
    if (j >= tokens->count) {
        return;
    }
    /* Must be `=` and not `==` */
    if (!token_is_symbol(&tokens->items[j], '=')) {
        return;
    }
    if (j + 1 < tokens->count && token_is_symbol(&tokens->items[j + 1], '=')) {
        return;
    }
    /* Skip to RHS */
    size_t k = j + 1;
    while (k < tokens->count && tokens->items[k].kind == TOKEN_NEWLINE) {
        k++;
    }
    if (k >= tokens->count) {
        return;
    }
    const token_t *rhs = &tokens->items[k];
    if (rhs->kind != TOKEN_IDENTIFIER) {
        return;
    }
    if (!token_is_owned_factory(rhs)) {
        return;
    }
    /* Register – ignore push failures silently (only cosmetic: free won't be injected) */
    raii_live_push(live, name, brace_depth);
}

/**
 * @brief Extract the first identifier from a `return` expression so we can
 *        skip freeing a variable that is being returned (ownership transfer).
 *
 * Scans forward from `return_index + 1` until end-of-line / `{` / EOF.
 * Returns a heap-allocated string or NULL if no identifier found.
 */
static char *extract_return_ident(const token_buffer_t *tokens, size_t return_index) {
    size_t i = return_index + 1;
    while (i < tokens->count) {
        const token_t *tok = &tokens->items[i];
        if (tok->kind == TOKEN_EOF || tok->kind == TOKEN_NEWLINE) {
            break;
        }
        if (tok->kind == TOKEN_IDENTIFIER) {
            char *copy = malloc(tok->length + 1);
            if (!copy) {
                return NULL;
            }
            memcpy(copy, tok->lexeme, tok->length);
            copy[tok->length] = '\0';
            return copy;
        }
        i++;
    }
    return NULL;
}


static bool analyze_fn_signature(const token_buffer_t *tokens,
                                 size_t fn_index,
                                 bool *has_explicit_return,
                                 bool *is_start_fn,
                                 bool *is_main_fn,
                                 diagnostic_t *diag) {
    bool skip_decorator_ident = false;
    size_t first_token = SIZE_MAX;
    size_t prev_token = SIZE_MAX;
    for (size_t j = fn_index + 1; j < tokens->count; ++j) {
        const token_t *cur = &tokens->items[j];
        if (cur->kind == TOKEN_EOF) {
            break;
        }
        if (cur->kind == TOKEN_NEWLINE) {
            continue;
        }
        if (cur->kind == TOKEN_AT) {
            skip_decorator_ident = true;
            continue;
        }
        if (skip_decorator_ident) {
            if (cur->kind == TOKEN_IDENTIFIER) {
                skip_decorator_ident = false;
                continue;
            }
            skip_decorator_ident = false;
        }
        if (first_token == SIZE_MAX) {
            first_token = j;
        }
        if (token_is_symbol(cur, '(')) {
            if (prev_token == SIZE_MAX) {
                continue;
            }
            const token_t *prev = &tokens->items[prev_token];
            if (prev->kind == TOKEN_IDENTIFIER &&
                !token_is_identifier(prev, "weird") &&
                !token_is_identifier(prev, "fn")) {
                if (has_explicit_return) {
                    *has_explicit_return = (first_token != SIZE_MAX && first_token != prev_token);
                }
                if (is_start_fn) {
                    *is_start_fn = token_is_identifier(prev, "start");
                }
                if (is_main_fn) {
                    *is_main_fn = token_is_identifier(prev, "main");
                }
                return true;
            }
        }
        prev_token = j;
    }
    diagnostic_set(diag, tokens->items[fn_index].line, "unable to parse fn signature");
    return false;
}

static bool handle_fn_keyword(FILE *out,
                              const char *source,
                              const token_buffer_t *tokens,
                              size_t *index,
                              size_t *last_emit,
                              bool *saw_start_fn,
                              bool *saw_main_fn,
                              diagnostic_t *diag) {
    bool has_explicit_return = false;
    bool is_start_fn = false;
    bool is_main_fn = false;
    if (!analyze_fn_signature(tokens,
                              *index,
                              &has_explicit_return,
                              &is_start_fn,
                              &is_main_fn,
                              diag)) {
        return false;
    }
    if (saw_start_fn && is_start_fn) {
        *saw_start_fn = true;
    }
    if (saw_main_fn && is_main_fn) {
        *saw_main_fn = true;
    }
    const token_t *fn_tok = &tokens->items[*index];
    if (!transpiler_copy_range(out, source, *last_emit, fn_tok->offset)) {
        diagnostic_set(diag, fn_tok->line, "failed to copy prefix before fn");
        return false;
    }
    if (!has_explicit_return) {
        if (fputs("int", out) < 0) {
            diagnostic_set(diag, fn_tok->line, "failed to emit implicit fn return type");
            return false;
        }
    }
    *last_emit = fn_tok->offset + fn_tok->length;
    return true;
}

/**
 * @brief Emit the macro prelude shared by every transpiled translation unit.
 */
static void emit_prelude(FILE *out) {
    fputs("#ifdef __cplusplus\nextern \"C\" {\n#endif\n", out);
    static const char *lines[] = {
        "#include <stdbool.h>\n",
        "#include <stddef.h>\n",
        "#include <stdint.h>\n",
        "#include <string.h>\n",
        "#include <stdio.h>\n",
        "#include <stdlib.h>\n",
        "#include <errno.h>\n",
        "#include <sys/types.h>\n",
        "#include <sys/socket.h>\n",
        "#include <netinet/in.h>\n",
        "#include <arpa/inet.h>\n",
        "#include <unistd.h>\n",
        "#include <pthread.h>\n",
        "#include <ttak/mem/mem.h>\n",
        "#include <ttak/mem/epoch_gc.h>\n",
        "#include <ttak/mem/detachable.h>\n",
        "#include <ttak/mem/epoch.h>\n",
        "#include <ttak/timing/timing.h>\n",
        "typedef char *string;\n",
        "#define auto __auto_type\n",
        "#define MARGO_CAT_IMPL(a, b) a##b\n",
        "#define MARGO_CAT(a, b) MARGO_CAT_IMPL(a, b)\n",
        "#define MARGO_WEIRD_0(T) T\n",
        "#define MARGO_WEIRD_1(T) T *\n",
        "#define MARGO_WEIRD_2(T) T **\n",
        "#define MARGO_WEIRD_3(T) T ***\n",
        "#define MARGO_WEIRD_4(T) T ****\n",
        "#define MARGO_WEIRD_5(T) T *****\n",
        "#define MARGO_WEIRD_6(T) T ******\n",
        "#define MARGO_WEIRD_7(T) T *******\n",
        "#define MARGO_WEIRD_8(T) T ********\n",
        "#define MARGO_WEIRD_SELECT(N) MARGO_CAT(MARGO_WEIRD_, N)\n",
        "#define weird(T, N) MARGO_WEIRD_SELECT(N)(T)\n",
        "typedef enum {\n",
        "    MARGO_PRINT_KIND_BOOL,\n",
        "    MARGO_PRINT_KIND_SIGNED,\n",
        "    MARGO_PRINT_KIND_UNSIGNED,\n",
        "    MARGO_PRINT_KIND_FLOAT,\n",
        "    MARGO_PRINT_KIND_CHAR,\n",
        "    MARGO_PRINT_KIND_STRING,\n",
        "    MARGO_PRINT_KIND_POINTER,\n",
        "} margo_print_kind_t;\n",
        "typedef struct {\n",
        "    margo_print_kind_t kind;\n",
        "    union {\n",
        "        bool boolean;\n",
        "        long long s64;\n",
        "        unsigned long long u64;\n",
        "        double f64;\n",
        "        const char *cstr;\n",
        "        const void *ptr;\n",
        "        char ch;\n",
        "    } data;\n",
        "} margo_print_value_t;\n",
        "static inline margo_print_value_t margo_print_value_from_bool(bool value) {\n",
        "    return (margo_print_value_t){ .kind = MARGO_PRINT_KIND_BOOL, .data.boolean = value };\n",
        "}\n",
        "static inline margo_print_value_t margo_print_value_from_signed(long long value) {\n",
        "    return (margo_print_value_t){ .kind = MARGO_PRINT_KIND_SIGNED, .data.s64 = value };\n",
        "}\n",
        "static inline margo_print_value_t margo_print_value_from_unsigned(unsigned long long value) {\n",
        "    return (margo_print_value_t){ .kind = MARGO_PRINT_KIND_UNSIGNED, .data.u64 = value };\n",
        "}\n",
        "static inline margo_print_value_t margo_print_value_from_double(double value) {\n",
        "    return (margo_print_value_t){ .kind = MARGO_PRINT_KIND_FLOAT, .data.f64 = value };\n",
        "}\n",
        "static inline margo_print_value_t margo_print_value_from_long_double(long double value) {\n",
        "    return margo_print_value_from_double((double)value);\n",
        "}\n",
        "static inline margo_print_value_t margo_print_value_from_char(char value) {\n",
        "    return (margo_print_value_t){ .kind = MARGO_PRINT_KIND_CHAR, .data.ch = value };\n",
        "}\n",
        "static inline margo_print_value_t margo_print_value_from_cstr(const char *text) {\n",
        "    return (margo_print_value_t){ .kind = MARGO_PRINT_KIND_STRING, .data.cstr = text };\n",
        "}\n",
        "static inline margo_print_value_t margo_print_value_from_pointer(const void *ptr) {\n",
        "    return (margo_print_value_t){ .kind = MARGO_PRINT_KIND_POINTER, .data.ptr = ptr };\n",
        "}\n",
"// string/size_t aliases resolve to the existing entries below, so avoid duplicates.\n",
"#define MARGO_PRINT_VALUE(value) _Generic((value), \\\n",
        "    bool: margo_print_value_from_bool, \\\n",
        "    char: margo_print_value_from_char, \\\n",
        "    signed char: margo_print_value_from_signed, \\\n",
        "    unsigned char: margo_print_value_from_unsigned, \\\n",
        "    short: margo_print_value_from_signed, \\\n",
        "    unsigned short: margo_print_value_from_unsigned, \\\n",
        "    int: margo_print_value_from_signed, \\\n",
        "    unsigned int: margo_print_value_from_unsigned, \\\n",
        "    long: margo_print_value_from_signed, \\\n",
"    unsigned long: margo_print_value_from_unsigned, \\\n",
"    long long: margo_print_value_from_signed, \\\n",
"    unsigned long long: margo_print_value_from_unsigned, \\\n",
        "    float: margo_print_value_from_double, \\\n",
        "    double: margo_print_value_from_double, \\\n",
        "    long double: margo_print_value_from_long_double, \\\n",
"    const char *: margo_print_value_from_cstr, \\\n",
"    char *: margo_print_value_from_cstr, \\\n",
        "    default: margo_print_value_from_pointer \\\n",
        ")(value)\n",
        "typedef struct {\n",
        "    bool is_char;\n",
        "    char ch;\n",
        "    const char *text;\n",
        "} margo_print_span_t;\n",
        "static inline margo_print_span_t margo_print_span_from_char(long long value) {\n",
        "    margo_print_span_t span = { .is_char = true, .ch = (char)value };\n",
        "    return span;\n",
        "}\n",
        "static inline margo_print_span_t margo_print_span_from_cstr(const char *text) {\n",
        "    margo_print_span_t span = { .is_char = false, .ch = 0, .text = text };\n",
        "    return span;\n",
        "}\n",
"// `_Generic` disallows duplicate compatible types; rely on pointer entries for aliases.\n",
"#define MARGO_PRINT_SPAN(value) _Generic((value), \\\n",
        "    char: margo_print_span_from_char, \\\n",
        "    signed char: margo_print_span_from_char, \\\n",
        "    unsigned char: margo_print_span_from_char, \\\n",
        "    short: margo_print_span_from_char, \\\n",
        "    unsigned short: margo_print_span_from_char, \\\n",
        "    int: margo_print_span_from_char, \\\n",
        "    unsigned int: margo_print_span_from_char, \\\n",
        "    long: margo_print_span_from_char, \\\n",
"    unsigned long: margo_print_span_from_char, \\\n",
"    long long: margo_print_span_from_char, \\\n",
"    unsigned long long: margo_print_span_from_char, \\\n",
"    const char *: margo_print_span_from_cstr, \\\n",
"    char *: margo_print_span_from_cstr \\\n",
        ")(value)\n",
        "static inline void margo_print_emit_span(margo_print_span_t span) {\n",
        "    if (span.is_char) {\n",
        "        fputc(span.ch, stdout);\n",
        "    } else if (span.text) {\n",
        "        fputs(span.text, stdout);\n",
        "    }\n",
        "}\n",
        "static inline void margo_print_emit_value(const margo_print_value_t *value) {\n",
        "    if (!value) {\n",
        "        return;\n",
        "    }\n",
        "    switch (value->kind) {\n",
        "        case MARGO_PRINT_KIND_BOOL:\n",
        "            fputs(value->data.boolean ? \"true\" : \"false\", stdout);\n",
        "            break;\n",
        "        case MARGO_PRINT_KIND_SIGNED:\n",
        "            fprintf(stdout, \"%lld\", value->data.s64);\n",
        "            break;\n",
        "        case MARGO_PRINT_KIND_UNSIGNED:\n",
        "            fprintf(stdout, \"%llu\", value->data.u64);\n",
        "            break;\n",
        "        case MARGO_PRINT_KIND_FLOAT:\n",
        "            fprintf(stdout, \"%g\", value->data.f64);\n",
        "            break;\n",
        "        case MARGO_PRINT_KIND_CHAR:\n",
        "            fputc(value->data.ch, stdout);\n",
        "            break;\n",
        "        case MARGO_PRINT_KIND_STRING:\n",
        "            if (value->data.cstr) {\n",
        "                fputs(value->data.cstr, stdout);\n",
        "            } else {\n",
        "                fputs(\"(null)\", stdout);\n",
        "            }\n",
        "            break;\n",
        "        case MARGO_PRINT_KIND_POINTER:\n",
        "            fprintf(stdout, \"%p\", value->data.ptr);\n",
        "            break;\n",
        "    }\n",
        "}\n",
        "static inline void margo_print_emit(const margo_print_value_t *values,\n",
        "                                    size_t value_count,\n",
        "                                    margo_print_span_t sep,\n",
        "                                    bool has_sep,\n",
        "                                    margo_print_span_t endl,\n",
        "                                    bool has_endl) {\n",
        "    margo_print_span_t active_sep = has_sep ? sep : margo_print_span_from_cstr(\" \");\n",
        "    margo_print_span_t active_endl = has_endl ? endl : margo_print_span_from_cstr(\"\\n\");\n",
        "    for (size_t idx = 0; idx < value_count; ++idx) {\n",
        "        if (idx > 0) {\n",
        "            margo_print_emit_span(active_sep);\n",
        "        }\n",
        "        margo_print_emit_value(values + idx);\n",
        "    }\n",
        "    margo_print_emit_span(active_endl);\n",
        "}\n",
        "typedef struct {\n",
        "    const char *fmt;\n",
        "    void *ptr;\n",
        "} margo_scan_param_t;\n",
        "static inline margo_scan_param_t margo_scan_param_make(const char *fmt, void *ptr) {\n",
        "    margo_scan_param_t param = { .fmt = fmt, .ptr = ptr };\n",
        "    return param;\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_signed_char(signed char *ptr) {\n",
        "    return margo_scan_param_make(\"%hhd\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_unsigned_char(unsigned char *ptr) {\n",
        "    return margo_scan_param_make(\"%hhu\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_short(short *ptr) {\n",
        "    return margo_scan_param_make(\"%hd\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_unsigned_short(unsigned short *ptr) {\n",
        "    return margo_scan_param_make(\"%hu\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_int(int *ptr) {\n",
        "    return margo_scan_param_make(\"%d\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_unsigned_int(unsigned int *ptr) {\n",
        "    return margo_scan_param_make(\"%u\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_long(long *ptr) {\n",
        "    return margo_scan_param_make(\"%ld\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_unsigned_long(unsigned long *ptr) {\n",
        "    return margo_scan_param_make(\"%lu\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_long_long(long long *ptr) {\n",
        "    return margo_scan_param_make(\"%lld\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_unsigned_long_long(unsigned long long *ptr) {\n",
        "    return margo_scan_param_make(\"%llu\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_size_t(size_t *ptr) {\n",
        "    return margo_scan_param_make(\"%zu\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_float(float *ptr) {\n",
        "    return margo_scan_param_make(\"%f\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_double(double *ptr) {\n",
        "    return margo_scan_param_make(\"%lf\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_long_double(long double *ptr) {\n",
        "    return margo_scan_param_make(\"%Lf\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_string(char *ptr) {\n",
        "    return margo_scan_param_make(\"%s\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_const_string(string ptr) {\n",
        "    return margo_scan_param_make(\"%s\", ptr);\n",
        "}\n",
        "static inline margo_scan_param_t margo_scan_param_unsupported(void *ptr) {\n",
        "    (void)ptr;\n",
        "    return margo_scan_param_make(NULL, NULL);\n",
        "}\n",
        "#define MARGO_SCAN_PARAM(value) _Generic((value), \\\n",
        "    signed char *: margo_scan_param_signed_char, \\\n",
        "    unsigned char *: margo_scan_param_unsigned_char, \\\n",
        "    short *: margo_scan_param_short, \\\n",
        "    unsigned short *: margo_scan_param_unsigned_short, \\\n",
        "    int *: margo_scan_param_int, \\\n",
        "    unsigned int *: margo_scan_param_unsigned_int, \\\n",
        "    long *: margo_scan_param_long, \\\n",
        "    unsigned long *: margo_scan_param_unsigned_long, \\\n",
        "    long long *: margo_scan_param_long_long, \\\n",
        "    unsigned long long *: margo_scan_param_unsigned_long_long, \\\n",
        "    float *: margo_scan_param_float, \\\n",
        "    double *: margo_scan_param_double, \\\n",
        "    long double *: margo_scan_param_long_double, \\\n",
        "    char *: margo_scan_param_string, \\\n",
        "    default: margo_scan_param_unsupported \\\n",
        ")(value)\n",
        "static inline int margo_scan_run(const margo_scan_param_t *params, size_t count, bool require_newline) {\n",
        "    if (!params && count > 0) {\n",
        "        return -1;\n",
        "    }\n",
        "    size_t matched = 0;\n",
        "    for (size_t i = 0; i < count; ++i) {\n",
        "        if (!params[i].fmt) {\n",
        "            return -1;\n",
        "        }\n",
        "        if (scanf(params[i].fmt, params[i].ptr) != 1) {\n",
        "            return (int)matched;\n",
        "        }\n",
        "        matched++;\n",
        "    }\n",
        "    if (require_newline) {\n",
        "        int ch = getchar();\n",
        "        while (ch != '\\n' && ch != EOF) {\n",
        "            ch = getchar();\n",
        "        }\n",
        "    }\n",
        "    return (int)matched;\n",
        "}\n",
        "static inline uint64_t margo_now_ticks(void) {\n",
        "    return ttak_get_tick_count();\n",
        "}\n",
        "typedef struct margo_fallback_alloc_node {\n",
        "    void *ptr;\n",
        "    struct margo_fallback_alloc_node *next;\n",
        "} margo_fallback_alloc_node_t;\n",
        "static margo_fallback_alloc_node_t *margo_fallback_alloc_head = NULL;\n",
        "static const size_t MARGO_DETACHABLE_MAX_BYTES = 256 * 1024;\n",
        "#define MARGO_DETACHABLE_MAGIC 0x4D4152474F444554ULL\n",
        "typedef struct {\n",
        "    uint64_t magic;\n",
        "    size_t requested;\n",
        "    ttak_detachable_context_t *ctx;\n",
        "    ttak_detachable_allocation_t allocation;\n",
        "} margo_detachable_header_t;\n",
        "typedef struct {\n",
        "    pthread_mutex_t lock;\n",
        "    bool initialized;\n",
        "    ttak_detachable_context_t ctx;\n",
        "} margo_detachable_runtime_t;\n",
        "static margo_detachable_runtime_t margo_detachable_runtime = {\n",
        "    .lock = PTHREAD_MUTEX_INITIALIZER,\n",
        "    .initialized = false,\n",
        "};\n",
        "static inline ttak_detachable_context_t *margo_detachable_context(void) {\n",
        "    if (margo_detachable_runtime.initialized) {\n",
        "        return &margo_detachable_runtime.ctx;\n",
        "    }\n",
        "    pthread_mutex_lock(&margo_detachable_runtime.lock);\n",
        "    if (!margo_detachable_runtime.initialized) {\n",
        "        ttak_detachable_context_init(&margo_detachable_runtime.ctx,\n",
        "            TTAK_ARENA_HAS_OWNER | TTAK_ARENA_HAS_EPOCH_RECLAMATION |\n",
        "            TTAK_ARENA_HAS_DEFAULT_EPOCH_GC | TTAK_ARENA_USE_LOCKED_ACCESS);\n",
        "        margo_detachable_runtime.initialized = true;\n",
        "    }\n",
        "    pthread_mutex_unlock(&margo_detachable_runtime.lock);\n",
        "    return &margo_detachable_runtime.ctx;\n",
        "}\n",
        "static inline margo_detachable_header_t *margo_detachable_header_from_payload(const void *ptr) {\n",
        "    if (!ptr) {\n",
        "        return NULL;\n",
        "    }\n",
        "    margo_detachable_header_t *header = ((margo_detachable_header_t *)ptr) - 1;\n",
        "    if (header->magic != MARGO_DETACHABLE_MAGIC) {\n",
        "        return NULL;\n",
        "    }\n",
        "    return header;\n",
        "}\n",
        "static pthread_once_t margo_epoch_tls_once = PTHREAD_ONCE_INIT;\n",
        "static pthread_key_t margo_epoch_tls_key;\n",
        "static _Thread_local bool margo_epoch_registered = false;\n",
        "static void margo_epoch_tls_cleanup(void *value) {\n",
        "    (void)value;\n",
        "    if (margo_epoch_registered) {\n",
        "        ttak_epoch_deregister_thread();\n",
        "        margo_epoch_registered = false;\n",
        "    }\n",
        "}\n",
        "static void margo_epoch_tls_init(void) {\n",
        "    pthread_key_create(&margo_epoch_tls_key, margo_epoch_tls_cleanup);\n",
        "}\n",
        "static inline void margo_epoch_ensure_registered(void) {\n",
        "    if (margo_epoch_registered) {\n",
        "        return;\n",
        "    }\n",
        "    pthread_once(&margo_epoch_tls_once, margo_epoch_tls_init);\n",
        "    ttak_epoch_register_thread();\n",
        "    pthread_setspecific(margo_epoch_tls_key, (void *)1);\n",
        "    margo_epoch_registered = true;\n",
        "}\n",
        "static void margo_epoch_free_callback(void *ptr) {\n",
        "    free(ptr);\n",
        "}\n",
        "static inline void margo_retire_fallback_ptr(void *ptr) {\n",
        "    if (!ptr) {\n",
        "        return;\n",
        "    }\n",
        "    margo_epoch_ensure_registered();\n",
        "    ttak_epoch_retire(ptr, margo_epoch_free_callback);\n",
        "    ttak_epoch_reclaim();\n",
        "}\n",
        "static inline bool margo_fallback_alloc_track(void *ptr) {\n",
        "    if (!ptr) {\n",
        "        return false;\n",
        "    }\n",
        "    margo_fallback_alloc_node_t *node = (margo_fallback_alloc_node_t *)malloc(sizeof(*node));\n",
        "    if (!node) {\n",
        "        errno = ENOMEM;\n",
        "        return false;\n",
        "    }\n",
        "    node->ptr = ptr;\n",
        "    node->next = margo_fallback_alloc_head;\n",
        "    margo_fallback_alloc_head = node;\n",
        "    return true;\n",
        "}\n",
        "static inline bool margo_fallback_alloc_untrack(void *ptr) {\n",
        "    if (!ptr) {\n",
        "        return false;\n",
        "    }\n",
        "    margo_fallback_alloc_node_t *prev = NULL;\n",
        "    margo_fallback_alloc_node_t *cur = margo_fallback_alloc_head;\n",
        "    while (cur) {\n",
        "        if (cur->ptr == ptr) {\n",
        "            if (prev) {\n",
        "                prev->next = cur->next;\n",
        "            } else {\n",
        "                margo_fallback_alloc_head = cur->next;\n",
        "            }\n",
        "            free(cur);\n",
        "            return true;\n",
        "        }\n",
        "        prev = cur;\n",
        "        cur = cur->next;\n",
        "    }\n",
        "    return false;\n",
        "}\n",
        "static inline void *margo_fallback_alloc(size_t bytes) {\n",
        "    void *ptr = malloc(bytes);\n",
        "    if (!ptr) {\n",
        "        return NULL;\n",
        "    }\n",
        "    if (!margo_fallback_alloc_track(ptr)) {\n",
        "        free(ptr);\n",
        "        return NULL;\n",
        "    }\n",
        "    return ptr;\n",
        "}\n",
        "static inline void margo_builtin_free_ptr(void *ptr) {\n",
        "    if (!ptr) {\n",
        "        return;\n",
        "    }\n",
        "    margo_detachable_header_t *header = margo_detachable_header_from_payload(ptr);\n",
        "    if (header) {\n",
        "        ttak_detachable_context_t *ctx = header->ctx;\n",
        "        ttak_detachable_allocation_t allocation = header->allocation;\n",
        "        ttak_detachable_mem_free(ctx, &allocation);\n",
        "        return;\n",
        "    }\n",
        "    if (margo_fallback_alloc_untrack(ptr)) {\n",
        "        margo_retire_fallback_ptr(ptr);\n",
        "        return;\n",
        "    }\n",
        "    free(ptr);\n",
        "}\n",
        "static inline void *margo_builtin_alloc_bytes(size_t bytes) {\n",
        "    if (bytes == 0) {\n",
        "        return NULL;\n",
        "    }\n",
        "    if (bytes > MARGO_DETACHABLE_MAX_BYTES) {\n",
        "        return margo_fallback_alloc(bytes);\n",
        "    }\n",
        "    margo_epoch_ensure_registered();\n",
        "    ttak_detachable_context_t *ctx = margo_detachable_context();\n",
        "    if (ctx) {\n",
        "        size_t total = bytes + sizeof(margo_detachable_header_t);\n",
        "        ttak_detachable_allocation_t allocation = ttak_detachable_mem_alloc(ctx, total, margo_now_ticks());\n",
        "        if (allocation.data) {\n",
        "            margo_detachable_header_t *header = (margo_detachable_header_t *)allocation.data;\n",
        "            header->magic = MARGO_DETACHABLE_MAGIC;\n",
        "            header->requested = bytes;\n",
        "            header->ctx = ctx;\n",
            "            header->allocation = allocation;\n",
            "            return (void *)(header + 1);\n",
        "        }\n",
        "    }\n",
        "    return margo_fallback_alloc(bytes);\n",
        "}\n",
        "static inline void *margo_builtin_alloc_and_copy(size_t bytes, const void *src, size_t src_len) {\n",
        "    void *dst = margo_builtin_alloc_bytes(bytes);\n",
        "    if (dst && src && src_len) {\n",
        "        size_t copy = src_len < bytes ? src_len : bytes;\n",
        "        memcpy(dst, src, copy);\n",
        "    }\n",
        "    return dst;\n",
        "}\n",
        "static inline size_t margo_file_read(void *dst, size_t elem_size, size_t elem_count, FILE *stream) {\n",
        "    if (!dst || !stream || elem_size == 0 || elem_count == 0) {\n",
        "        return 0;\n",
        "    }\n",
        "    return fread(dst, elem_size, elem_count, stream);\n",
        "}\n",
        "static inline size_t margo_file_write(const void *src, size_t elem_size, size_t elem_count, FILE *stream) {\n",
        "    if (!src || !stream || elem_size == 0 || elem_count == 0) {\n",
        "        return 0;\n",
        "    }\n",
        "    return fwrite(src, elem_size, elem_count, stream);\n",
        "}\n",
        "static inline int margo_net_tcp_connect(const char *ipv4, uint16_t port) {\n",
        "    if (!ipv4 || !*ipv4) {\n",
        "        errno = EINVAL;\n",
        "        return -1;\n",
        "    }\n",
        "    int sock = socket(AF_INET, SOCK_STREAM, 0);\n",
        "    if (sock < 0) {\n",
        "        return -1;\n",
        "    }\n",
        "    struct sockaddr_in addr;\n",
        "    memset(&addr, 0, sizeof(addr));\n",
        "    addr.sin_family = AF_INET;\n",
        "    addr.sin_port = htons(port);\n",
        "    if (inet_pton(AF_INET, ipv4, &addr.sin_addr) != 1) {\n",
        "        close(sock);\n",
        "        errno = EINVAL;\n",
        "        return -1;\n",
        "    }\n",
        "    if (connect(sock, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {\n",
        "        int saved = errno;\n",
        "        close(sock);\n",
        "        errno = saved;\n",
        "        return -1;\n",
        "    }\n",
        "    return sock;\n",
        "}\n",
        "static inline ssize_t margo_net_send(int sock, const void *buf, size_t len) {\n",
        "    if (sock < 0 || (!buf && len > 0)) {\n",
        "        errno = EINVAL;\n",
        "        return -1;\n",
        "    }\n",
        "    return send(sock, buf, len, 0);\n",
        "}\n",
        "static inline ssize_t margo_net_recv(int sock, void *buf, size_t len) {\n",
        "    if (sock < 0 || (!buf && len > 0)) {\n",
        "        errno = EINVAL;\n",
        "        return -1;\n",
        "    }\n",
        "    return recv(sock, buf, len, 0);\n",
        "}\n",
        "static inline void margo_net_close(int sock) {\n",
        "    if (sock >= 0) {\n",
        "        close(sock);\n",
        "    }\n",
        "}\n",
        "#define alloc(size) margo_builtin_alloc_bytes((size_t)(size))\n",
        "#define alloc_and_init(size, literal) margo_builtin_alloc_and_copy((size_t)(size), (literal), sizeof(literal))\n",
        NULL,
    };
    for (size_t i = 0; lines[i]; ++i) {
        fputs(lines[i], out);
    }
    fputs("#ifdef __cplusplus\n}\n#endif\n", out);
}

/**
 * @brief Decide whether a line-ending token needs an implicit semicolon.
 */
static bool should_insert_semicolon(char last_char) {
    if (last_char == '\0') {
        return false;
    }
    switch (last_char) {
        case ';':
        case '{':
        case '}':
        case ',':
            return false;
        default:
            return true;
    }
}

/**
 * @brief Decide whether a line ending with `last` (preceded by `prev`)
 *        continues onto the next physical line, in which case no implicit
 *        semicolon may be inserted.
 *
 * This keeps semicolons optional even for multi-line macros (`\`
 * continuations), expressions split after an operator, and calls split
 * after an open bracket.  Postfix `x++`/`x--` and float literals such as
 * `1.` still count as complete statements.
 */
static bool line_ends_with_continuation(char last, char prev) {
    switch (last) {
        case '\\':
        case '(':
        case '[':
        case '=':
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case '<':
        case '>':
        case '!':
        case '~':
        case '?':
        case ':':
            break;
        case '.':
            if (prev >= '0' && prev <= '9') {
                return false;
            }
            break;
        default:
            return false;
    }
    if ((last == '+' || last == '-') && prev == last) {
        return false;
    }
    return true;
}

/**
 * @brief Decide whether `ident` can lead a line whose `)`-terminated header
 *        owns a following `{` block (control-flow keywords, plus the
 *        type/storage tokens that can start a function definition).
 */
static bool line_starts_header_keyword(const char *ident) {
    static const char *keywords[] = {
        "if",       "for",     "while",   "switch",
        "static",   "extern",  "inline",  "const",
        "unsigned", "signed",  "long",    "short",
        "int",      "char",    "float",   "double",
        "void",     "struct",  "union",   "enum",
        "auto",     "weird",   "string",  "size_t",
        "bool",     "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "int8_t",  "int16_t",  "int32_t",
        "int64_t",
    };
    if (!ident || !ident[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if (strcmp(ident, keywords[i]) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Scan past whitespace and comments after `pos` for an opening brace.
 *
 * Allman-style headers (`if (cond)\n{`, function definitions, etc.) must
 * not receive a semicolon after their closing parenthesis.
 */
static bool next_significant_is_open_brace(const char *src, size_t len, size_t pos) {
    size_t j = pos;
    while (j < len) {
        char c = src[j];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            j++;
            continue;
        }
        if (c == '/' && j + 1 < len && src[j + 1] == '/') {
            while (j < len && src[j] != '\n') {
                j++;
            }
            continue;
        }
        if (c == '/' && j + 1 < len && src[j + 1] == '*') {
            j += 2;
            while (j + 1 < len && !(src[j] == '*' && src[j + 1] == '/')) {
                j++;
            }
            if (j + 1 < len) {
                j += 2;
            }
            continue;
        }
        return c == '{';
    }
    return false;
}

/**
 * @brief Convert concept-style newline terminators into real semicolons for C.
 */
static bool insert_implicit_semicolons(char **buffer, size_t *size, diagnostic_t *diag) {
    if (!buffer || !*buffer || !size) {
        return true;
    }
    const char *src = *buffer;
    size_t len = *size;
    size_t cap = (len * 2) + 1;
    char *out = malloc(cap);
    if (!out) {
        diagnostic_set(diag, 0, "out of memory while normalizing statements");
        return false;
    }
    size_t w = 0;
    bool start_of_line = true;
    bool line_is_directive = false;
    bool directive_continuation = false;
    bool in_user_section = false;
    bool has_pending_user_state = false;
    bool pending_user_state = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    bool in_char_literal = false;
    bool string_escape = false;
    bool char_escape = false;
    bool block_comment_closing_tail = false;
    char last_non_ws = '\0';
    char prev_non_ws = '\0';
    char prev_char = '\0';
    bool line_has_assignment = false;
    bool line_closed_aggregate = false;
    bool pending_aggregate_keyword = false;
    int pending_aggregate_paren_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    int *aggregate_stack = NULL;
    size_t aggregate_stack_count = 0;
    size_t aggregate_stack_capacity = 0;
    bool in_identifier = false;
    char ident_buf[64];
    size_t ident_len = 0;
    char line_first_ident[64] = {0};
    bool line_first_ident_valid = false;
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        char next = (i + 1 < len) ? src[i + 1] : '\0';
        if (c == '\r' || c == '\n') {
            bool ends_with_backslash = (last_non_ws == '\\');
            if (line_is_directive && has_pending_user_state) {
                in_user_section = pending_user_state;
            } else if (!line_is_directive && !directive_continuation &&
                       !ends_with_backslash &&
                       in_user_section && last_non_ws != '\0') {
                bool need_semicolon = should_insert_semicolon(last_non_ws);
                if (!need_semicolon && last_non_ws == '}' &&
                    (line_has_assignment || line_closed_aggregate)) {
                    need_semicolon = true;
                }
                if (need_semicolon && paren_depth > 0) {
                    need_semicolon = false;
                }
                if (need_semicolon &&
                    line_ends_with_continuation(last_non_ws, prev_non_ws)) {
                    need_semicolon = false;
                }
                if (need_semicolon && last_non_ws == ')' &&
                    line_first_ident_valid &&
                    line_starts_header_keyword(line_first_ident) &&
                    next_significant_is_open_brace(src, len, i)) {
                    need_semicolon = false;
                }
                if (need_semicolon) {
                    out[w++] = ';';
                }
            }
            directive_continuation =
                (line_is_directive || directive_continuation) && ends_with_backslash;
            if (c == '\r') {
                out[w++] = '\r';
                if (next == '\n') {
                    out[w++] = '\n';
                    i++;
                }
            } else {
                out[w++] = '\n';
            }
            start_of_line = true;
            line_is_directive = false;
            has_pending_user_state = false;
            pending_user_state = false;
            in_line_comment = false;
            last_non_ws = '\0';
            prev_non_ws = '\0';
            line_has_assignment = false;
            prev_char = '\0';
            line_closed_aggregate = false;
            line_first_ident_valid = false;
            continue;
        }
        if (start_of_line) {
            if (c == ' ' || c == '\t') {
                out[w++] = c;
                continue;
            }
            start_of_line = false;
            if (!in_block_comment && !in_line_comment && c == '#') {
                line_is_directive = true;
                size_t probe = i + 1;
                while (probe < len && (src[probe] == ' ' || src[probe] == '\t')) {
                    probe++;
                }
                if (probe + 4 <= len && strncmp(src + probe, "line", 4) == 0) {
                    probe += 4;
                    while (probe < len && (src[probe] == ' ' || src[probe] == '\t')) {
                        probe++;
                    }
                    bool negative = false;
                    if (probe < len && src[probe] == '-') {
                        negative = true;
                        probe++;
                    }
                    long value = 0;
                    bool has_digits = false;
                    while (probe < len && src[probe] >= '0' && src[probe] <= '9') {
                        has_digits = true;
                        value = value * 10 + (src[probe] - '0');
                        probe++;
                    }
                    if (has_digits && !negative) {
                        pending_user_state = value > 0;
                        has_pending_user_state = true;
                    }
                }
            } else {
                line_is_directive = false;
            }
        }
        bool part_of_comment_delim = false;
        if (block_comment_closing_tail) {
            part_of_comment_delim = true;
            block_comment_closing_tail = false;
        } else if (!in_string && !in_char_literal) {
            if (!in_line_comment && !in_block_comment) {
                if (c == '/' && next == '/') {
                    in_line_comment = true;
                    part_of_comment_delim = true;
                } else if (c == '/' && next == '*') {
                    in_block_comment = true;
                    part_of_comment_delim = true;
                }
            } else if (in_block_comment && c == '*' && next == '/') {
                part_of_comment_delim = true;
                in_block_comment = false;
                block_comment_closing_tail = true;
            }
        }
        if (!in_line_comment && !in_block_comment) {
            if (in_string) {
                if (!string_escape && c == '\\') {
                    string_escape = true;
                } else if (string_escape) {
                    string_escape = false;
                } else if (c == '"') {
                    in_string = false;
                }
            } else if (in_char_literal) {
                if (!char_escape && c == '\\') {
                    char_escape = true;
                } else if (char_escape) {
                    char_escape = false;
                } else if (c == '\'') {
                    in_char_literal = false;
                }
            } else {
                if (c == '"') {
                    in_string = true;
                } else if (c == '\'') {
                    in_char_literal = true;
                }
            }
        } else {
            string_escape = false;
            char_escape = false;
        }
        out[w++] = c;
        if (!in_line_comment && !in_block_comment && !part_of_comment_delim && !isspace((unsigned char)c)) {
            prev_non_ws = last_non_ws;
            last_non_ws = c;
        }
        if (!in_line_comment && !in_block_comment && !in_string && !in_char_literal) {
            if (c == '=' && next != '=' && prev_char != '<' && prev_char != '>' &&
                prev_char != '!' && prev_char != '=') {
                line_has_assignment = true;
            }
            if (pending_aggregate_keyword) {
                if (c == ';' || c == ',') {
                    pending_aggregate_keyword = false;
                }
            }
            bool is_ident_char = (c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                  (c >= 'A' && c <= 'Z'));
            if (is_ident_char) {
                if (!in_identifier) {
                    in_identifier = true;
                    ident_len = 0;
                }
                if (ident_len + 1 < sizeof(ident_buf)) {
                    ident_buf[ident_len++] = (char)tolower((unsigned char)c);
                }
            } else if (in_identifier) {
                ident_buf[ident_len] = '\0';
                if (!line_first_ident_valid) {
                    memcpy(line_first_ident, ident_buf, ident_len + 1);
                    line_first_ident_valid = true;
                }
                if (strcmp(ident_buf, "struct") == 0 || strcmp(ident_buf, "union") == 0 ||
                    strcmp(ident_buf, "enum") == 0) {
                    pending_aggregate_keyword = true;
                    pending_aggregate_paren_depth = paren_depth;
                }
                in_identifier = false;
                ident_len = 0;
            }
            if (!is_ident_char) {
                in_identifier = false;
                ident_len = 0;
            }
            if (c == '(') {
                paren_depth++;
            } else if (c == ')') {
                if (paren_depth > 0) {
                    paren_depth--;
                    if (pending_aggregate_keyword && paren_depth < pending_aggregate_paren_depth) {
                        pending_aggregate_keyword = false;
                    }
                }
            }
            if (c == '{') {
                brace_depth++;
                if (pending_aggregate_keyword && paren_depth == pending_aggregate_paren_depth) {
                    if (aggregate_stack_count == aggregate_stack_capacity) {
                        size_t new_capacity = aggregate_stack_capacity ? aggregate_stack_capacity * 2 : 8;
                        int *new_items = realloc(aggregate_stack, new_capacity * sizeof(int));
                        if (!new_items) {
                            free(out);
                            free(aggregate_stack);
                            diagnostic_set(diag, 0, "out of memory while tracking aggregate braces");
                            return false;
                        }
                        aggregate_stack = new_items;
                        aggregate_stack_capacity = new_capacity;
                    }
                    aggregate_stack[aggregate_stack_count++] = brace_depth;
                    pending_aggregate_keyword = false;
                }
            } else if (c == '}') {
                if (brace_depth > 0) {
                    if (aggregate_stack_count > 0 &&
                        aggregate_stack[aggregate_stack_count - 1] == brace_depth) {
                        aggregate_stack_count--;
                        line_closed_aggregate = true;
                    }
                    brace_depth--;
                }
            }
        }
        if (!in_line_comment && !in_block_comment && !part_of_comment_delim && !isspace((unsigned char)c)) {
            prev_char = c;
        }
    }
    if (in_user_section && !line_is_directive && !directive_continuation &&
        !in_line_comment && !in_block_comment) {
        bool need_semicolon = should_insert_semicolon(last_non_ws);
        if (!need_semicolon && last_non_ws == '}' &&
            (line_has_assignment || line_closed_aggregate)) {
            need_semicolon = true;
        }
        if (need_semicolon && paren_depth > 0) {
            need_semicolon = false;
        }
        if (need_semicolon &&
            line_ends_with_continuation(last_non_ws, prev_non_ws)) {
            need_semicolon = false;
        }
        if (need_semicolon) {
            out[w++] = ';';
        }
    }
    out[w] = '\0';
    free(aggregate_stack);
    free(*buffer);
    *buffer = out;
    *size = w;
    return true;
}

/**
 * @brief Convert an `@import` directive into a concrete `#include` line (or
 *        a set of includes for `godmode`).
 */
static bool emit_include_for_import(FILE *out, const import_directive_t *dir, diagnostic_t *diag) {
    if (dir->kind == IMPORT_KIND_C) {
        const char *name = dir->target;
        size_t len = strlen(name);
        const char *last_slash = strrchr(name, '/');
        const char *last_backslash = strrchr(name, '\\');
        const char *path_sep = last_slash;
        if (!path_sep || (last_backslash && last_backslash > path_sep)) {
            path_sep = last_backslash;
        }
        const char *last_dot = strrchr(name, '.');
        bool has_suffix = false;
        if (last_dot && last_dot > (path_sep ? path_sep : name)) {
            has_suffix = true;
        }
        bool needs_quotes = false;
        if (name[0] == '/' || name[0] == '.' || name[0] == '\\') {
            needs_quotes = true;
        } else if (strchr(name, '/') || strchr(name, '\\')) {
            needs_quotes = true;
        }
        const char *fmt = NULL;
        if (needs_quotes) {
            fmt = has_suffix ? "#include \"%s\"\n" : "#include \"%s.h\"\n";
        } else {
            fmt = has_suffix ? "#include <%s>\n" : "#include <%s.h>\n";
        }
        if (fprintf(out, fmt, name) < 0) {
            diagnostic_set(diag, 0, "failed to emit include for %s", name);
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_CPP) {
        /* C++ binding support: emit the include inside an extern "C" guard
         * so that C++ headers can be included safely in Margo sources. */
        const char *name = dir->target;
        if (fprintf(out, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n") < 0 ||
            fprintf(out, "#include <%s>\n", name) < 0 ||
            fprintf(out, "#ifdef __cplusplus\n}\n#endif\n") < 0) {
            diagnostic_set(diag, 0, "failed to emit c++ import");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_GODMODE) {
        /* Expand to a comprehensive set of standard C headers so that any
         * Margo program can access the full C standard library without
         * enumerating individual @import directives.  This is intentionally
         * generous to support writing the self-hosting compiler in Margo. */
        static const char *godmode_headers[] = {
            "#include <assert.h>\n",
            "#include <ctype.h>\n",
            "#include <errno.h>\n",
            "#include <float.h>\n",
            "#include <limits.h>\n",
            "#include <math.h>\n",
            "#include <setjmp.h>\n",
            "#include <signal.h>\n",
            "#include <stdarg.h>\n",
            "#include <stdbool.h>\n",
            "#include <stddef.h>\n",
            "#include <stdint.h>\n",
            "#include <stdio.h>\n",
            "#include <stdlib.h>\n",
            "#include <string.h>\n",
            "#include <time.h>\n",
            "#include <inttypes.h>\n",
            "#include <fcntl.h>\n",
            "#include <unistd.h>\n",
            "#include <sys/types.h>\n",
            "#include <sys/stat.h>\n",
            NULL,
        };
        for (size_t hi = 0; godmode_headers[hi]; ++hi) {
            if (fputs(godmode_headers[hi], out) == EOF) {
                diagnostic_set(diag, 0, "failed to emit godmode includes");
                return false;
            }
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD_STRING) {
        if (fputs("#include <string.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit std/string include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD_MATH) {
        if (fputs("#include <math.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit std/math include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD_STDLIB) {
        if (fputs("#include <stdlib.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit std/stdlib include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD_TIME) {
        if (fputs("#include <time.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit std/time include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD_ASSERT) {
        if (fputs("#include <assert.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit std/assert include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD_ERRNO) {
        if (fputs("#include <errno.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit std/errno include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD_FILE) {
        if (fputs("#include <stdio.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit std/file include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_FILE_CORE) {
        if (fputs("#include <stdio.h>\n", out) == EOF ||
            fputs("#include <errno.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit file/core includes");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_NETWORK_CORE) {
        if (fputs("#include <sys/types.h>\n", out) == EOF ||
            fputs("#include <sys/socket.h>\n", out) == EOF ||
            fputs("#include <netinet/in.h>\n", out) == EOF ||
            fputs("#include <arpa/inet.h>\n", out) == EOF ||
            fputs("#include <unistd.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit network/core includes");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_THREADS_CORE) {
        if (fputs("#include <margo_threads/core.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit threads/core include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_PROCESS_CORE) {
        if (fputs("#include <margo_process/core.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit process/core include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MATRIX_CORE) {
        if (fputs("#include <margo_matrix/core.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit matrix/core include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MARGO_STD_VECTOR) {
        if (fputs("#include <margo_std/vector.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit margo_std/vector include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MARGO_STD_HASHMAP) {
        if (fputs("#include <margo_std/hashmap.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit margo_std/hashmap include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MARGO_STD_RESULT) {
        if (fputs("#include <margo_std/result.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit margo_std/result include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MARGO_STD_OPTIONAL) {
        if (fputs("#include <margo_std/optional.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit margo_std/optional include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MARGO_STD_STRING_BUILDER) {
        if (fputs("#include <margo_std/string_builder.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit margo_std/string_builder include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MARGO_STD_SLICE) {
        if (fputs("#include <margo_std/slice.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit margo_std/slice include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_MARGO_STD_OWNED) {
        if (fputs("#include <margo_std/owned.h>\n", out) == EOF) {
            diagnostic_set(diag, 0, "failed to emit margo_std/owned include");
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_STD) {
        const char *mapped = NULL;
        if (strcmp(dir->target, "io") == 0) {
            mapped = "<stdio.h>";
        } else if (strcmp(dir->target, "mem") == 0) {
            mapped = "<string.h>";
        }
        if (!mapped) {
            diagnostic_set(diag, 0, "unknown std module '%s'", dir->target);
            return false;
        }
        if (fprintf(out, "#include %s\n", mapped) < 0) {
            diagnostic_set(diag, 0, "failed to emit std include for %s", dir->target);
            return false;
        }
        return true;
    }
    diagnostic_set(diag, 0, "unsupported import kind");
    return false;
}

/**
 * @brief Wrap arbitrary directive text inside a block comment.
 */
static bool write_comment(FILE *out, const char *text_start, size_t text_len) {
    if (fprintf(out, "/* %.*s */", (int)text_len, text_start) < 0) {
        return false;
    }
    return true;
}

/**
 * @brief Trim whitespace at the beginning of a range.
 */
static size_t trim_range_start(const char *src, size_t start, size_t end) {
    while (start < end && isspace((unsigned char)src[start])) {
        start++;
    }
    return start;
}

/**
 * @brief Trim trailing whitespace from a range.
 */
static size_t trim_range_end(const char *src, size_t start, size_t end) {
    while (end > start && isspace((unsigned char)src[end - 1])) {
        end--;
    }
    return end;
}

/* `def NAME = expression` is parsed as a declaration, not text substitution.
 * C validates the emitted enum value as an integer constant expression. */
static bool handle_def_declaration(FILE *out,
                                   const char *source,
                                   const token_buffer_t *tokens,
                                   size_t *index,
                                   size_t *last_emit,
                                   diagnostic_t *diag) {
    size_t i = *index;
    size_t name_index = i + 1;
    if (name_index >= tokens->count || tokens->items[name_index].kind != TOKEN_IDENTIFIER) {
        diagnostic_set(diag, tokens->items[i].line, "def requires a name");
        return false;
    }
    size_t equals_index = name_index + 1;
    if (equals_index >= tokens->count || !token_is_symbol(&tokens->items[equals_index], '=')) {
        diagnostic_set(diag, tokens->items[i].line, "def requires '=' after its name");
        return false;
    }
    size_t expr_index = equals_index + 1;
    if (expr_index >= tokens->count || tokens->items[expr_index].kind == TOKEN_NEWLINE ||
        tokens->items[expr_index].kind == TOKEN_EOF) {
        diagnostic_set(diag, tokens->items[i].line, "def requires a constant expression");
        return false;
    }
    int depth = 0;
    size_t end_index = expr_index;
    for (; end_index < tokens->count; ++end_index) {
        const token_t *tok = &tokens->items[end_index];
        if (tok->kind == TOKEN_EOF || tok->kind == TOKEN_NEWLINE ||
            (depth == 0 && token_is_symbol(tok, ';'))) {
            break;
        }
        if (token_is_symbol(tok, '(') || token_is_symbol(tok, '[') || token_is_symbol(tok, '{')) {
            depth++;
        } else if ((token_is_symbol(tok, ')') || token_is_symbol(tok, ']') || token_is_symbol(tok, '}')) && depth > 0) {
            depth--;
        }
    }
    if (end_index == expr_index || depth != 0) {
        diagnostic_set(diag, tokens->items[i].line, "def has an invalid constant expression");
        return false;
    }
    size_t expr_start = trim_range_start(source, tokens->items[expr_index].offset,
                                         tokens->items[end_index - 1].offset + tokens->items[end_index - 1].length);
    size_t expr_end = trim_range_end(source, expr_start,
                                     tokens->items[end_index - 1].offset + tokens->items[end_index - 1].length);
    if (expr_start == expr_end ||
        !transpiler_copy_range(out, source, *last_emit, tokens->items[i].offset) ||
        fputs("enum { ", out) == EOF ||
        fwrite(tokens->items[name_index].lexeme, 1, tokens->items[name_index].length, out) != tokens->items[name_index].length ||
        fputs(" = (", out) == EOF ||
        !transpiler_copy_range(out, source, expr_start, expr_end) ||
        fputs(") };", out) == EOF) {
        diagnostic_set(diag, tokens->items[i].line, "failed to emit def declaration");
        return false;
    }
    *last_emit = (end_index < tokens->count && token_is_symbol(&tokens->items[end_index], ';'))
                     ? tokens->items[end_index].offset + tokens->items[end_index].length
                     : expr_end;
    *index = end_index > 0 ? end_index - 1 : i;
    return true;
}

static bool range_has_non_whitespace(const char *src, size_t start, size_t end) {
    while (start < end) {
        if (!isspace((unsigned char)src[start])) {
            return true;
        }
        start++;
    }
    return false;
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} print_argument_list_t;

static void print_argument_list_free(print_argument_list_t *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool print_argument_list_append(print_argument_list_t *list, char *expr) {
    if (!list || !expr) {
        return false;
    }
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity ? list->capacity * 2 : 4;
        char **new_items = realloc(list->items, new_capacity * sizeof(char *));
        if (!new_items) {
            return false;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = expr;
    return true;
}

static ptrdiff_t find_print_assignment_equals(const char *source, size_t start, size_t end) {
    int paren_depth = 0;
    int brace_depth = 0;
    int bracket_depth = 0;
    bool in_string = false;
    bool in_char = false;
    bool string_escape = false;
    bool char_escape = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    for (size_t i = start; i < end; ++i) {
        char c = source[i];
        char next = (i + 1 < end) ? source[i + 1] : '\0';
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
            }
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                i++;
            }
            continue;
        }
        if (in_string) {
            if (string_escape) {
                string_escape = false;
            } else if (c == '\\') {
                string_escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (in_char) {
            if (char_escape) {
                char_escape = false;
            } else if (c == '\\') {
                char_escape = true;
            } else if (c == '\'') {
                in_char = false;
            }
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            i++;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            i++;
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '\'') {
            in_char = true;
            continue;
        }
        if (c == '(') {
            paren_depth++;
            continue;
        }
        if (c == ')' && paren_depth > 0) {
            paren_depth--;
            continue;
        }
        if (c == '{') {
            brace_depth++;
            continue;
        }
        if (c == '}' && brace_depth > 0) {
            brace_depth--;
            continue;
        }
        if (c == '[') {
            bracket_depth++;
            continue;
        }
        if (c == ']' && bracket_depth > 0) {
            bracket_depth--;
            continue;
        }
        if (c == '=' && paren_depth == 0 && brace_depth == 0 && bracket_depth == 0) {
            char prev = (i > start) ? source[i - 1] : '\0';
            char following = next;
            if (prev == '=' || prev == '<' || prev == '>' || prev == '!' || prev == '+' || prev == '-' ||
                prev == '*' || prev == '/' || prev == '%' || prev == '&' || prev == '|' || prev == '^' ||
                following == '=') {
                continue;
            }
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

static bool process_print_argument_range(const char *source,
                                         size_t start,
                                         size_t end,
                                         print_argument_list_t *positional,
                                         char **sep_expr,
                                         char **endl_expr,
                                         bool *saw_named,
                                         size_t error_line,
                                         diagnostic_t *diag) {
    size_t trimmed_start = trim_range_start(source, start, end);
    size_t trimmed_end = trim_range_end(source, trimmed_start, end);
    if (trimmed_start == trimmed_end) {
        return true;
    }
    ptrdiff_t eq_pos = find_print_assignment_equals(source, trimmed_start, trimmed_end);
    if (eq_pos >= 0) {
        size_t eq_offset = (size_t)eq_pos;
        size_t ident_start = trim_range_start(source, trimmed_start, eq_offset);
        size_t ident_end = trim_range_end(source, ident_start, eq_offset);
        size_t ident_len = ident_end > ident_start ? ident_end - ident_start : 0;
        bool is_sep = ident_len == 3 && strncmp(source + ident_start, "sep", 3) == 0;
        bool is_endl = ident_len == 4 && strncmp(source + ident_start, "endl", 4) == 0;
        if (is_sep || is_endl) {
            if (*saw_named == false) {
                *saw_named = true;
            }
            size_t value_start = trim_range_start(source, eq_offset + 1, trimmed_end);
            if (value_start >= trimmed_end) {
                diagnostic_set(diag, error_line, "print named argument is missing a value");
                return false;
            }
            char *value = duplicate_range(source, value_start, trimmed_end);
            if (!value) {
                diagnostic_set(diag, error_line, "out of memory while capturing print option");
                return false;
            }
            if (is_sep) {
                if (*sep_expr) {
                    free(value);
                    diagnostic_set(diag, error_line, "print sep specified more than once");
                    return false;
                }
                *sep_expr = value;
            } else {
                if (*endl_expr) {
                    free(value);
                    diagnostic_set(diag, error_line, "print endl specified more than once");
                    return false;
                }
                *endl_expr = value;
            }
            return true;
        }
    }
    if (*saw_named) {
        diagnostic_set(diag, error_line, "print positional arguments must precede named arguments");
        return false;
    }
    char *expr = duplicate_range(source, trimmed_start, trimmed_end);
    if (!expr) {
        diagnostic_set(diag, error_line, "out of memory while capturing print argument");
        return false;
    }
    if (!print_argument_list_append(positional, expr)) {
        free(expr);
        diagnostic_set(diag, error_line, "out of memory while storing print argument");
        return false;
    }
    return true;
}

static bool handle_print_call(FILE *out,
                              const char *source,
                              const token_buffer_t *tokens,
                              size_t *index,
                              size_t *last_emit,
                              diagnostic_t *diag) {
    size_t i = *index;
    size_t paren_index = i + 1;
    while (paren_index < tokens->count && tokens->items[paren_index].kind == TOKEN_NEWLINE) {
        paren_index++;
    }
    if (paren_index >= tokens->count || !token_is_symbol(&tokens->items[paren_index], '(')) {
        return true;
    }
    print_argument_list_t positional = {0};
    char *sep_expr = NULL;
    char *endl_expr = NULL;
    bool saw_named = false;
    bool ok = true;
    size_t arg_start = tokens->items[paren_index].offset + tokens->items[paren_index].length;
    size_t closing_index = SIZE_MAX;
    int depth = 0;
    for (size_t j = paren_index; ok && j < tokens->count; ++j) {
        const token_t *cur = &tokens->items[j];
        if (cur->kind == TOKEN_EOF) {
            break;
        }
        if (token_is_symbol(cur, '(')) {
            depth++;
            continue;
        }
        if (token_is_symbol(cur, ')')) {
            if (depth == 0) {
                diagnostic_set(diag, cur->line, "unexpected ')' in print call");
                ok = false;
                break;
            }
            depth--;
            if (depth == 0) {
                size_t arg_end = cur->offset;
                if (!process_print_argument_range(source,
                                                  arg_start,
                                                  arg_end,
                                                  &positional,
                                                  &sep_expr,
                                                  &endl_expr,
                                                  &saw_named,
                                                  tokens->items[i].line,
                                                  diag)) {
                    ok = false;
                    break;
                }
                closing_index = j;
                break;
            }
            continue;
        }
        if (token_is_symbol(cur, ',') && depth == 1) {
            size_t arg_end = cur->offset;
            if (!process_print_argument_range(source,
                                              arg_start,
                                              arg_end,
                                              &positional,
                                              &sep_expr,
                                              &endl_expr,
                                              &saw_named,
                                              tokens->items[i].line,
                                              diag)) {
                ok = false;
                break;
            }
            arg_start = cur->offset + cur->length;
            continue;
        }
    }
    if (ok && closing_index == SIZE_MAX) {
        diagnostic_set(diag, tokens->items[i].line, "unterminated print call");
        ok = false;
    }
    if (!ok) {
        print_argument_list_free(&positional);
        free(sep_expr);
        free(endl_expr);
        return false;
    }
    if (!transpiler_copy_range(out, source, *last_emit, tokens->items[i].offset)) {
        diagnostic_set(diag, tokens->items[i].line, "failed to copy prefix before print call");
        print_argument_list_free(&positional);
        free(sep_expr);
        free(endl_expr);
        return false;
    }
    if (positional.count > 0) {
        if (fputs("margo_print_emit((margo_print_value_t[]){", out) < 0) {
            ok = false;
        }
        for (size_t arg_idx = 0; ok && arg_idx < positional.count; ++arg_idx) {
            if (arg_idx > 0 && fputs(", ", out) < 0) {
                ok = false;
                break;
            }
            if (fputs("MARGO_PRINT_VALUE(", out) < 0 ||
                fputs(positional.items[arg_idx], out) < 0 ||
                fputs(")", out) < 0) {
                ok = false;
                break;
            }
        }
        if (ok && fprintf(out, "}, %zu, ", positional.count) < 0) {
            ok = false;
        }
    } else {
        if (fprintf(out, "margo_print_emit(NULL, %zu, ", (size_t)0) < 0) {
            ok = false;
        }
    }
    if (ok) {
        if (sep_expr) {
            if (fputs("MARGO_PRINT_SPAN(", out) < 0 ||
                fputs(sep_expr, out) < 0 ||
                fputs("), true, ", out) < 0) {
                ok = false;
            }
        } else if (fputs("(margo_print_span_t){0}, false, ", out) < 0) {
            ok = false;
        }
    }
    if (ok) {
        if (endl_expr) {
            if (fputs("MARGO_PRINT_SPAN(", out) < 0 ||
                fputs(endl_expr, out) < 0 ||
                fputs("), true)", out) < 0) {
                ok = false;
            }
        } else if (fputs("(margo_print_span_t){0}, false)", out) < 0) {
            ok = false;
        }
    }
    if (!ok) {
        diagnostic_set(diag, tokens->items[i].line, "failed to emit rewritten print call");
        print_argument_list_free(&positional);
        free(sep_expr);
        free(endl_expr);
        return false;
    }
    *last_emit = tokens->items[closing_index].offset + tokens->items[closing_index].length;
    *index = closing_index;
    print_argument_list_free(&positional);
    free(sep_expr);
    free(endl_expr);
    return true;
}

static bool handle_scan_call(FILE *out,
                             const char *source,
                             const token_buffer_t *tokens,
                             size_t *index,
                             size_t *last_emit,
                             bool require_newline,
                             bool needs_address_of,
                             bool *used_std_io,
                             size_t *std_io_line,
                             diagnostic_t *diag) {
    size_t i = *index;
    size_t paren_index = i + 1;
    while (paren_index < tokens->count && tokens->items[paren_index].kind == TOKEN_NEWLINE) {
        paren_index++;
    }
    if (paren_index >= tokens->count || !token_is_symbol(&tokens->items[paren_index], '(')) {
        return true;
    }
    print_argument_list_t args = {0};
    bool ok = true;
    size_t arg_start = tokens->items[paren_index].offset + tokens->items[paren_index].length;
    size_t closing_index = SIZE_MAX;
    int depth = 0;
    for (size_t j = paren_index; ok && j < tokens->count; ++j) {
        const token_t *cur = &tokens->items[j];
        if (cur->kind == TOKEN_EOF) {
            break;
        }
        if (token_is_symbol(cur, '(')) {
            depth++;
            continue;
        }
        if (token_is_symbol(cur, ')')) {
            if (depth == 0) {
                diagnostic_set(diag, cur->line, "unexpected ')' in Scan call");
                ok = false;
                break;
            }
            depth--;
            if (depth == 0) {
                size_t arg_end = cur->offset;
                size_t trimmed_start = trim_range_start(source, arg_start, arg_end);
                size_t trimmed_end = trim_range_end(source, trimmed_start, arg_end);
                if (trimmed_start != trimmed_end) {
                    char *expr = duplicate_range(source, trimmed_start, trimmed_end);
                    if (!expr) {
                        diagnostic_set(diag, cur->line, "out of memory while capturing Scan argument");
                        ok = false;
                        break;
                    }
                    if (!print_argument_list_append(&args, expr)) {
                        free(expr);
                        diagnostic_set(diag, cur->line, "out of memory while storing Scan argument");
                        ok = false;
                        break;
                    }
                } else if (range_has_non_whitespace(source, arg_start, arg_end)) {
                    diagnostic_set(diag, cur->line, "Scan argument is empty");
                    ok = false;
                    break;
                }
                closing_index = j;
                break;
            }
            continue;
        }
        if (token_is_symbol(cur, ',') && depth == 1) {
            size_t arg_end = cur->offset;
            size_t trimmed_start = trim_range_start(source, arg_start, arg_end);
            size_t trimmed_end = trim_range_end(source, trimmed_start, arg_end);
            if (trimmed_start != trimmed_end) {
                char *expr = duplicate_range(source, trimmed_start, trimmed_end);
                if (!expr) {
                    diagnostic_set(diag, cur->line, "out of memory while capturing Scan argument");
                    ok = false;
                    break;
                }
                if (!print_argument_list_append(&args, expr)) {
                    free(expr);
                    diagnostic_set(diag, cur->line, "out of memory while storing Scan argument");
                    ok = false;
                    break;
                }
            } else if (range_has_non_whitespace(source, arg_start, arg_end)) {
                diagnostic_set(diag, cur->line, "Scan argument is empty");
                ok = false;
                break;
            }
            arg_start = cur->offset + cur->length;
            continue;
        }
    }
    if (ok && closing_index == SIZE_MAX) {
        diagnostic_set(diag, tokens->items[i].line, "unterminated Scan call");
        ok = false;
    }
    if (!ok) {
        print_argument_list_free(&args);
        return false;
    }
    if (!transpiler_copy_range(out, source, *last_emit, tokens->items[i].offset)) {
        diagnostic_set(diag, tokens->items[i].line, "failed to copy prefix before Scan call");
        print_argument_list_free(&args);
        return false;
    }
    if (args.count > 0) {
        if (fputs("margo_scan_run((margo_scan_param_t[]){", out) < 0) {
            diagnostic_set(diag, tokens->items[i].line, "failed to emit Scan array");
            print_argument_list_free(&args);
            return false;
        }
        for (size_t arg_idx = 0; arg_idx < args.count; ++arg_idx) {
            if (arg_idx > 0 && fputs(", ", out) < 0) {
                diagnostic_set(diag, tokens->items[i].line, "failed to separate Scan arguments");
                print_argument_list_free(&args);
                return false;
            }
            if (fputs("MARGO_SCAN_PARAM(", out) < 0 ||
                (needs_address_of && fputs("&", out) < 0) ||
                fputs(args.items[arg_idx], out) < 0 ||
                fputs(")", out) < 0) {
                diagnostic_set(diag, tokens->items[i].line, "failed to emit Scan argument");
                print_argument_list_free(&args);
                return false;
            }
        }
        if (fprintf(out, "}, %zu, %s)", args.count, require_newline ? "true" : "false") < 0) {
            diagnostic_set(diag, tokens->items[i].line, "failed to finalize Scan call");
            print_argument_list_free(&args);
            return false;
        }
    } else {
        if (fprintf(out, "margo_scan_run(NULL, (size_t)0, %s)", require_newline ? "true" : "false") < 0) {
            diagnostic_set(diag, tokens->items[i].line, "failed to emit empty Scan call");
            print_argument_list_free(&args);
            return false;
        }
    }
    *last_emit = tokens->items[closing_index].offset + tokens->items[closing_index].length;
    *index = closing_index;
    if (used_std_io) {
        *used_std_io = true;
    }
    if (std_io_line && *std_io_line == 0) {
        *std_io_line = tokens->items[i].line;
    }
    print_argument_list_free(&args);
    return true;
}

/**
 * @brief Decide whether `out` / `in` at position `i` is used as a statement
 *        keyword rather than an ordinary identifier (member access such as
 *        `obj.out` / `ctx->in` is left untouched).
 */
static bool io_keyword_preceded_by_member_access(const token_buffer_t *tokens, size_t i) {
    if (i == 0) {
        return false;
    }
    const token_t *prev = &tokens->items[i - 1];
    return token_is_symbol(prev, '.') || token_is_symbol(prev, '-') ||
           token_is_symbol(prev, '>');
}

/**
 * @brief Rewrite an `out expr % expr ...` statement.
 *
 * When the first operand is a string literal and at least one `%`-chained
 * argument follows, the statement lowers to `printf(fmt, args...)`, giving
 * full printf-style formatting (a superset of iostream manipulators).
 * Otherwise it lowers to a cout-style `margo_print_emit` call with no
 * separators and no trailing newline.  Inside an `out` statement `%` acts
 * as the chain operator; write `(a % b)` for modulo.
 *
 * Returns true without consuming tokens when `out` is a plain identifier.
 */
static bool handle_out_statement(FILE *out,
                                 const char *source,
                                 const token_buffer_t *tokens,
                                 size_t *index,
                                 size_t *last_emit,
                                 diagnostic_t *diag) {
    size_t i = *index;
    if (io_keyword_preceded_by_member_access(tokens, i)) {
        return true;
    }
    size_t cursor = i + 1;
    if (cursor >= tokens->count) {
        return true;
    }
    const token_t *first = &tokens->items[cursor];
    bool starts_expr =
        first->kind == TOKEN_IDENTIFIER || first->kind == TOKEN_NUMBER ||
        first->kind == TOKEN_STRING || first->kind == TOKEN_CHAR ||
        token_is_symbol(first, '(');
    if (!starts_expr) {
        return true;
    }
    /* Split segments at top-level `%`; the statement ends at the first
     * newline that does not directly follow a chain operator. */
    print_argument_list_t segments = {0};
    bool ok = true;
    size_t seg_start = first->offset;
    size_t last_index = cursor;
    int depth = 0;
    size_t j = cursor;
    for (; ok && j < tokens->count; ++j) {
        const token_t *cur = &tokens->items[j];
        if (cur->kind == TOKEN_EOF) {
            break;
        }
        if (cur->kind == TOKEN_NEWLINE && depth == 0) {
            size_t prev = j - 1;
            while (prev > cursor && tokens->items[prev].kind == TOKEN_NEWLINE) {
                prev--;
            }
            if (token_is_symbol(&tokens->items[prev], '%')) {
                continue;
            }
            break;
        }
        if (token_is_symbol(cur, ';') && depth == 0) {
            break;
        }
        if (token_is_symbol(cur, '(') || token_is_symbol(cur, '[') ||
            token_is_symbol(cur, '{')) {
            depth++;
            continue;
        }
        if (token_is_symbol(cur, ')') || token_is_symbol(cur, ']') ||
            token_is_symbol(cur, '}')) {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        if (token_is_symbol(cur, '%') && depth == 0) {
            size_t trimmed_start = trim_range_start(source, seg_start, cur->offset);
            size_t trimmed_end = trim_range_end(source, trimmed_start, cur->offset);
            if (trimmed_start == trimmed_end) {
                diagnostic_set(diag, cur->line, "out operand is empty");
                ok = false;
                break;
            }
            char *expr = duplicate_range(source, trimmed_start, trimmed_end);
            if (!expr || !print_argument_list_append(&segments, expr)) {
                free(expr);
                diagnostic_set(diag, cur->line, "out of memory while capturing out operand");
                ok = false;
                break;
            }
            seg_start = cur->offset + cur->length;
            continue;
        }
    }
    if (ok) {
        last_index = j - 1;
        const token_t *last_tok = &tokens->items[last_index];
        size_t seg_end = last_tok->offset + last_tok->length;
        size_t trimmed_start = trim_range_start(source, seg_start, seg_end);
        size_t trimmed_end = trim_range_end(source, trimmed_start, seg_end);
        if (trimmed_start == trimmed_end) {
            diagnostic_set(diag, tokens->items[i].line, "out operand is empty");
            ok = false;
        } else {
            char *expr = duplicate_range(source, trimmed_start, trimmed_end);
            if (!expr || !print_argument_list_append(&segments, expr)) {
                free(expr);
                diagnostic_set(diag, tokens->items[i].line,
                               "out of memory while capturing out operand");
                ok = false;
            }
        }
    }
    if (!ok) {
        print_argument_list_free(&segments);
        return false;
    }
    if (!transpiler_copy_range(out, source, *last_emit, tokens->items[i].offset)) {
        diagnostic_set(diag, tokens->items[i].line, "failed to copy prefix before out statement");
        print_argument_list_free(&segments);
        return false;
    }
    /* printf form: a pure leading string literal that carries a conversion
     * specifier, with chained arguments.  Literals without '%' are treated
     * as cout-style pieces so `out "x=" % x` prints the value. */
    bool literal_is_format = false;
    if (segments.count >= 2 && first->kind == TOKEN_STRING &&
        segments.items[0][0] == '"') {
        for (const char *p = segments.items[0]; *p; ++p) {
            if (*p == '%') {
                literal_is_format = true;
                break;
            }
        }
    }
    if (literal_is_format) {
        if (fputs("printf(", out) < 0 || fputs(segments.items[0], out) < 0) {
            ok = false;
        }
        for (size_t arg_idx = 1; ok && arg_idx < segments.count; ++arg_idx) {
            if (fputs(", ", out) < 0 || fputs(segments.items[arg_idx], out) < 0) {
                ok = false;
            }
        }
        if (ok && fputs(")", out) < 0) {
            ok = false;
        }
    } else {
        if (fputs("margo_print_emit((margo_print_value_t[]){", out) < 0) {
            ok = false;
        }
        for (size_t arg_idx = 0; ok && arg_idx < segments.count; ++arg_idx) {
            if (arg_idx > 0 && fputs(", ", out) < 0) {
                ok = false;
                break;
            }
            if (fputs("MARGO_PRINT_VALUE(", out) < 0 ||
                fputs(segments.items[arg_idx], out) < 0 ||
                fputs(")", out) < 0) {
                ok = false;
                break;
            }
        }
        if (ok &&
            fprintf(out, "}, %zu, MARGO_PRINT_SPAN(\"\"), true, MARGO_PRINT_SPAN(\"\"), true)",
                    segments.count) < 0) {
            ok = false;
        }
    }
    if (!ok) {
        diagnostic_set(diag, tokens->items[i].line, "failed to emit rewritten out statement");
        print_argument_list_free(&segments);
        return false;
    }
    *last_emit = tokens->items[last_index].offset + tokens->items[last_index].length;
    *index = last_index;
    print_argument_list_free(&segments);
    return true;
}

/**
 * @brief Rewrite an `in x y ...` statement into a type-dispatched scan.
 *
 * Each operand must be an identifier; the statement lowers to
 * `margo_scan_run` with `MARGO_SCAN_PARAM(&ident)` per operand, mirroring
 * `cin >> x >> y`.  Requires `@import std/io` (shared with `Scan`).
 *
 * Returns true without consuming tokens when `in` is a plain identifier.
 */
static bool handle_in_statement(FILE *out,
                                const char *source,
                                const token_buffer_t *tokens,
                                size_t *index,
                                size_t *last_emit,
                                bool *used_std_io,
                                size_t *std_io_line,
                                diagnostic_t *diag) {
    size_t i = *index;
    if (io_keyword_preceded_by_member_access(tokens, i)) {
        return true;
    }
    size_t cursor = i + 1;
    if (cursor >= tokens->count ||
        tokens->items[cursor].kind != TOKEN_IDENTIFIER) {
        return true;
    }
    print_argument_list_t vars = {0};
    bool ok = true;
    size_t j = cursor;
    for (; ok && j < tokens->count; ++j) {
        const token_t *cur = &tokens->items[j];
        if (cur->kind == TOKEN_EOF || cur->kind == TOKEN_NEWLINE ||
            token_is_symbol(cur, ';')) {
            break;
        }
        if (token_is_symbol(cur, ',')) {
            continue;
        }
        if (cur->kind != TOKEN_IDENTIFIER) {
            diagnostic_set(diag, cur->line, "in expects identifiers as operands");
            ok = false;
            break;
        }
        char *name = duplicate_range(source, cur->offset, cur->offset + cur->length);
        if (!name || !print_argument_list_append(&vars, name)) {
            free(name);
            diagnostic_set(diag, cur->line, "out of memory while capturing in operand");
            ok = false;
            break;
        }
    }
    if (!ok) {
        print_argument_list_free(&vars);
        return false;
    }
    if (!transpiler_copy_range(out, source, *last_emit, tokens->items[i].offset)) {
        diagnostic_set(diag, tokens->items[i].line, "failed to copy prefix before in statement");
        print_argument_list_free(&vars);
        return false;
    }
    if (fputs("margo_scan_run((margo_scan_param_t[]){", out) < 0) {
        ok = false;
    }
    for (size_t var_idx = 0; ok && var_idx < vars.count; ++var_idx) {
        if (var_idx > 0 && fputs(", ", out) < 0) {
            ok = false;
            break;
        }
        if (fputs("MARGO_SCAN_PARAM(&", out) < 0 ||
            fputs(vars.items[var_idx], out) < 0 ||
            fputs(")", out) < 0) {
            ok = false;
            break;
        }
    }
    if (ok && fprintf(out, "}, %zu, false)", vars.count) < 0) {
        ok = false;
    }
    if (!ok) {
        diagnostic_set(diag, tokens->items[i].line, "failed to emit rewritten in statement");
        print_argument_list_free(&vars);
        return false;
    }
    size_t last_index = j - 1;
    *last_emit = tokens->items[last_index].offset + tokens->items[last_index].length;
    *index = last_index;
    if (used_std_io) {
        *used_std_io = true;
    }
    if (std_io_line && *std_io_line == 0) {
        *std_io_line = tokens->items[i].line;
    }
    print_argument_list_free(&vars);
    return true;
}

/**
 * @brief Rewrite `@style` directives into comments while preserving spacing.
 */
static bool handle_style_directive(FILE *out,
                                   const char *source,
                                   const token_buffer_t *tokens,
                                   size_t *index,
                                   size_t *last_emit,
                                   int brace_depth,
                                   style_block_stack_t *style_blocks,
                                   diagnostic_t *diag) {
    size_t i = *index;
    size_t cursor = i + 2;
    size_t last_token = i + 1;
    while (cursor < tokens->count) {
        const token_t *tok = &tokens->items[cursor];
        if (tok->kind == TOKEN_NEWLINE || token_is_symbol(tok, '{')) {
            break;
        }
        last_token = cursor;
        cursor++;
    }
    if (cursor >= tokens->count) {
        diagnostic_set(diag, tokens->items[i].line, "unterminated @style directive");
        return false;
    }
    size_t directive_start = tokens->items[i].offset;
    size_t directive_end = tokens->items[last_token].offset + tokens->items[last_token].length;
    size_t whitespace_end = directive_end;
    bool block_follows = false;
    if (tokens->items[cursor].kind == TOKEN_NEWLINE) {
        whitespace_end = tokens->items[cursor].offset + tokens->items[cursor].length;
    } else {
        whitespace_end = tokens->items[cursor].offset;
        if (token_is_symbol(&tokens->items[cursor], '{')) {
            block_follows = true;
        }
    }
    if (!transpiler_copy_range(out, source, *last_emit, directive_start)) {
        diagnostic_set(diag, 0, "failed to write source prefix");
        return false;
    }
    size_t trimmed_start = trim_range_start(source, directive_start, directive_end);
    size_t trimmed_end = trim_range_end(source, trimmed_start, directive_end);
    if (!write_comment(out, source + trimmed_start, trimmed_end - trimmed_start)) {
        diagnostic_set(diag, 0, "failed to emit @style comment");
        return false;
    }
    if (!transpiler_copy_range(out, source, directive_end, whitespace_end)) {
        diagnostic_set(diag, 0, "failed to copy spacing after @style");
        return false;
    }
    if (block_follows) {
        if (!style_stack_push(style_blocks, brace_depth, brace_depth == 0)) {
            diagnostic_set(diag, tokens->items[i].line, "out of memory while tracking @style block");
            return false;
        }
    }
    *last_emit = whitespace_end;
    *index = cursor - 1;
    return true;
}

/**
 * @brief Same as `handle_style_directive` but for `@set`.
 */
static bool handle_set_directive(FILE *out,
                                 const char *source,
                                 const token_buffer_t *tokens,
                                 size_t *index,
                                 size_t *last_emit,
                                 diagnostic_t *diag) {
    size_t i = *index;
    size_t cursor = i + 2;
    size_t last_token = i + 1;
    while (cursor < tokens->count) {
        const token_t *tok = &tokens->items[cursor];
        if (tok->kind == TOKEN_NEWLINE || tok->kind == TOKEN_EOF) {
            break;
        }
        last_token = cursor;
        cursor++;
    }
    size_t directive_start = tokens->items[i].offset;
    size_t directive_end = tokens->items[last_token].offset + tokens->items[last_token].length;
    size_t newline_end = directive_end;
    if (cursor < tokens->count && tokens->items[cursor].kind == TOKEN_NEWLINE) {
        newline_end = tokens->items[cursor].offset + tokens->items[cursor].length;
    }
    if (!transpiler_copy_range(out, source, *last_emit, directive_start)) {
        diagnostic_set(diag, 0, "failed to copy prefix before @set");
        return false;
    }
    size_t trimmed_start = trim_range_start(source, directive_start, directive_end);
    size_t trimmed_end = trim_range_end(source, trimmed_start, directive_end);
    if (!write_comment(out, source + trimmed_start, trimmed_end - trimmed_start)) {
        diagnostic_set(diag, 0, "failed to emit @set comment");
        return false;
    }
    if (newline_end > directive_end) {
        if (!transpiler_copy_range(out, source, directive_end, newline_end)) {
            diagnostic_set(diag, 0, "failed to append newline after @set");
            return false;
        }
    }
    *last_emit = newline_end;
    *index = (cursor < tokens->count) ? cursor - 1 : tokens->count - 1;
    return true;
}

/**
 * @brief Convert decorators such as `fn @foo bar()` into harmless comments.
 */
static bool handle_decorator(FILE *out,
                             const char *source,
                             const token_buffer_t *tokens,
                             size_t *index,
                             size_t *last_emit,
                             diagnostic_t *diag) {
    size_t i = *index;
    if (i + 1 >= tokens->count) {
        diagnostic_set(diag, tokens->items[i].line, "dangling decorator");
        return false;
    }
    const token_t *ident = &tokens->items[i + 1];
    size_t start = tokens->items[i].offset;
    size_t end = ident->offset + ident->length;
    if (!transpiler_copy_range(out, source, *last_emit, start)) {
        diagnostic_set(diag, 0, "failed to copy prefix before decorator");
        return false;
    }
    size_t trimmed_start = trim_range_start(source, start, end);
    size_t trimmed_end = trim_range_end(source, trimmed_start, end);
    if (!write_comment(out, source + trimmed_start, trimmed_end - trimmed_start)) {
        diagnostic_set(diag, 0, "failed to emit decorator comment");
        return false;
    }
    *last_emit = end;
    *index = i + 1;
    return true;
}

/**
 * @brief Entry point used by the builder to convert `.margo` to C source.
 *
 * The function now performs two sub-passes before the main token-rewriting
 * loop:
 *   1. Semantic analysis (`sema_run`) – builds the RAII ownership table and
 *      collects any type-level diagnostics.  Diagnostics are printed to
 *      stderr but do not abort compilation unless they are fatal.
 *   2. Main rewriting pass – translates Margo syntax to C while injecting
 *      scope-exit `free()` calls for alloc-owned variables and lowering
 *      the `null` keyword.
 */
bool margo_transpile_to_buffer(const char *input_path, char **buffer_out, size_t *size_out, diagnostic_t *diag) {
    diagnostic_clear(diag);
    char *source = NULL;
    size_t source_len = 0;
    if (!flatten_entry_file(input_path, &source, &source_len, diag)) {
        return false;
    }
    token_buffer_t tokens;
    if (!lexer_tokenize(source, source_len, &tokens, diag)) {
        free(source);
        return false;
    }

    bool weird_changed = false;
    if (!rewrite_weird_struct_args(&source, &source_len, &tokens, diag, &weird_changed)) {
        lexer_free(&tokens);
        free(source);
        return false;
    }
    if (weird_changed) {
        lexer_free(&tokens);
        if (!lexer_tokenize(source, source_len, &tokens, diag)) {
            free(source);
            return false;
        }
    }

    /* ---- Semantic analysis pass ---------------------------------------- */
    raii_table_t   sema_raii   = {0};
    sema_diag_list_t sema_diags = {0};
    if (!sema_run(source, &tokens, &sema_raii, &sema_diags, diag)) {
        /* Fatal internal error (OOM etc.) */
        raii_table_free(&sema_raii);
        sema_diag_list_free(&sema_diags);
        lexer_free(&tokens);
        free(source);
        return false;
    }
    if (sema_diags.count > 0) {
        sema_diag_list_print(&sema_diags, input_path);
    }
    /* Check error count before freeing the list */
    size_t sema_error_count = sema_diags.error_count;
    sema_diag_list_free(&sema_diags);
    raii_table_free(&sema_raii);

    if (sema_error_count > 0) {
        diagnostic_set(diag, 0, "aborting due to %zu semantic error(s)", sema_error_count);
        lexer_free(&tokens);
        free(source);
        return false;
    }

    /* Runtime RAII live tracker – populated inline during the code-gen pass */
    raii_live_t raii_live = {0};
    defer_stack_t defer_stack = {0};

    /* ---- Code generation pass ------------------------------------------ */
    char *generated = NULL;
    size_t generated_size = 0;
    FILE *out = open_memstream(&generated, &generated_size);
    if (!out) {
        diagnostic_set(diag, 0, "failed to create output buffer");
        raii_live_free(&raii_live);
        lexer_free(&tokens);
        free(source);
        return false;
    }
    emit_prelude(out);
    fprintf(out, "#line 1 \"%s\"\n", input_path);
    size_t last_emit = 0;
    bool ok = true;
    int brace_depth = 0;
    /* Depth at which the current function body started (for return-path
     * frees).  0 means we are at file scope. */
    int fn_body_depth = 0;
    style_block_stack_t style_blocks = {0};
    blocked_if_stack_t blocked_ifs = {0};
    bool has_start_function = false;
    bool has_main_function = false;
    bool std_io_imported = false;
    bool std_io_used = false;
    size_t std_io_use_line = 0;
    /* Pending for-in array: when we see the opening brace of a for-in array
       loop, we inject `auto var = iterable[idx];` right after it. */
    bool pending_for_in_array = false;
    char pending_for_in_var[64] = {0};
    char *pending_for_in_iterable = NULL;
    for (size_t i = 0; ok && i < tokens.count; ++i) {
        token_t *tok = &tokens.items[i];
        if (tok->kind == TOKEN_EOF) {
            break;
        }

        /* `null` → `NULL` */
        if (token_is_identifier(tok, "null")) {
            if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                diagnostic_set(diag, tok->line, "failed to copy prefix before null");
                ok = false;
                break;
            }
            if (fputs("NULL", out) == EOF) {
                diagnostic_set(diag, tok->line, "failed to emit NULL");
                ok = false;
                break;
            }
            last_emit = tok->offset + tok->length;
            continue;
        }

        if (token_is_identifier(tok, "fn")) {
            if (!handle_fn_keyword(out, source, &tokens, &i, &last_emit, &has_start_function, &has_main_function, diag)) {
                ok = false;
            }
            continue;
        }
        if (token_is_identifier(tok, "def")) {
            if (!handle_def_declaration(out, source, &tokens, &i, &last_emit, diag)) {
                ok = false;
            }
            continue;
        }
        if (tok->kind == TOKEN_AT) {
            if (i + 1 < tokens.count) {
                token_t *next = &tokens.items[i + 1];
                if (token_is_identifier(next, "import")) {
                    import_directive_t dir;
                    if (!parser_parse_import(source, &tokens, i, &dir, diag)) {
                        ok = false;
                        break;
                    }
                    bool is_std_io =
                        (dir.kind == IMPORT_KIND_STD && dir.target && strcmp(dir.target, "io") == 0) ||
                        dir.kind == IMPORT_KIND_GODMODE;
                    if (!transpiler_copy_range(out, source, last_emit, dir.start_offset)) {
                        diagnostic_set(diag, 0, "failed to copy source before @import");
                        parser_free_import(&dir);
                        ok = false;
                        break;
                    }
                    if (!emit_include_for_import(out, &dir, diag)) {
                        parser_free_import(&dir);
                        ok = false;
                        break;
                    }
                    last_emit = dir.end_offset;
                    i = dir.next_index ? dir.next_index - 1 : i;
                    if (is_std_io) {
                        std_io_imported = true;
                    }
                    parser_free_import(&dir);
                    continue;
                }
                if (token_is_identifier(next, "style")) {
                    if (!handle_style_directive(out, source, &tokens, &i, &last_emit, brace_depth, &style_blocks, diag)) {
                        ok = false;
                    }
                    continue;
                }
                if (token_is_identifier(next, "set")) {
                    if (!handle_set_directive(out, source, &tokens, &i, &last_emit, diag)) {
                        ok = false;
                    }
                    continue;
                }
            }
            if (!handle_decorator(out, source, &tokens, &i, &last_emit, diag)) {
                ok = false;
            }
            continue;
        }
        if (token_is_identifier(tok, "print")) {
            if (!handle_print_call(out, source, &tokens, &i, &last_emit, diag)) {
                ok = false;
                break;
            }
            continue;
        }
        if (token_is_identifier(tok, "Scan") || token_is_identifier(tok, "ScanLine")) {
            bool require_newline = token_is_identifier(tok, "ScanLine");
            bool needs_address_of = token_is_identifier(tok, "Scan");
            if (!handle_scan_call(out, source, &tokens, &i, &last_emit, require_newline, needs_address_of, &std_io_used, &std_io_use_line, diag)) {
                ok = false;
                break;
            }
            continue;
        }
        if (token_is_identifier(tok, "out")) {
            if (!handle_out_statement(out, source, &tokens, &i, &last_emit, diag)) {
                ok = false;
                break;
            }
            continue;
        }
        if (token_is_identifier(tok, "in")) {
            if (!handle_in_statement(out, source, &tokens, &i, &last_emit, &std_io_used, &std_io_use_line, diag)) {
                ok = false;
                break;
            }
            continue;
        }
        if (token_is_identifier(tok, "for")) {
            size_t lookahead = i + 1;
            while (lookahead < tokens.count && tokens.items[lookahead].kind == TOKEN_NEWLINE) {
                lookahead++;
            }
            if (lookahead < tokens.count && token_is_symbol(&tokens.items[lookahead], '(')) {
                continue;
            }
            /* Try regular for header first, then for-in */
            for_header_t header;
            bool is_regular_for = parser_parse_for_header(source, &tokens, i, &header, diag);
            if (is_regular_for) {
                if (!transpiler_copy_range(out, source, last_emit, header.start_offset)) {
                    diagnostic_set(diag, 0, "failed to copy prefix before for loop");
                    parser_free_for_header(&header);
                    ok = false;
                    break;
                }
                if (header.has_initializer) {
                    fprintf(out, "for (%s; %s; %s)", header.initializer, header.condition, header.increment);
                } else {
                    fprintf(out, "for (; %s; %s)", header.condition, header.increment);
                }
                fputs(header.trailing_ws, out);
                last_emit = header.block_offset;
                i = header.next_index ? header.next_index - 1 : i;
                parser_free_for_header(&header);
                continue;
            }
            /* Reset diagnostic and try for-in */
            diagnostic_clear(diag);
            for_in_header_t in_header;
            if (parser_parse_for_in_header(source, &tokens, i, &in_header, diag)) {
                if (!transpiler_copy_range(out, source, last_emit, in_header.start_offset)) {
                    diagnostic_set(diag, 0, "failed to copy prefix before for-in loop");
                    parser_free_for_in_header(&in_header);
                    ok = false;
                    break;
                }
                if (in_header.kind == FOR_IN_KIND_RANGE) {
                    const char *op = in_header.range_inclusive ? "<=" : "<";
                    fprintf(out, "for (auto %s = %s; %s %s %s; ++%s)",
                            in_header.loop_var, in_header.range_start,
                            in_header.loop_var, op, in_header.range_end,
                            in_header.loop_var);
                } else {
                    /* array iteration – emit the for header, then wait for
                       the opening brace to inject the loop variable. */
                    fprintf(out, "for (size_t _margo_i = 0; _margo_i < (sizeof(%s)/sizeof((%s)[0])); ++_margo_i)",
                            in_header.iterable, in_header.iterable);
                    pending_for_in_array = true;
                    size_t vlen = strlen(in_header.loop_var);
                    if (vlen >= sizeof(pending_for_in_var)) {
                        vlen = sizeof(pending_for_in_var) - 1;
                    }
                    memcpy(pending_for_in_var, in_header.loop_var, vlen);
                    pending_for_in_var[vlen] = '\0';
                    free(pending_for_in_iterable);
                    pending_for_in_iterable = tp_strdup(in_header.iterable);
                    last_emit = in_header.block_offset;
                    i = in_header.next_index ? in_header.next_index - 1 : i;
                    parser_free_for_in_header(&in_header);
                    continue;
                }
                fputs(in_header.trailing_ws, out);
                last_emit = in_header.block_offset;
                i = in_header.next_index ? in_header.next_index - 1 : i;
                parser_free_for_in_header(&in_header);
                continue;
            }
            ok = false;
            break;
        }
        if (token_is_identifier(tok, "threads")) {
            threads_block_t block;
            if (!parser_parse_threads_block(source, &tokens, i, &block, diag)) {
                ok = false;
                break;
            }
            if (!transpiler_copy_range(out, source, last_emit, block.start_offset)) {
                parser_free_threads_block(&block);
                ok = false;
                break;
            }
            /* Emit each thread body as a static C function */
            for (size_t t = 0; t < block.thread_count; ++t) {
                thread_def_t *td = &block.threads[t];
                fprintf(out, "\nstatic void %s_%s_body(int id, void *arg) {\n    (void)id; (void)arg;\n",
                        block.cluster_name, td->name);
                size_t body_content_start = td->body_start_offset + 1;
                size_t body_content_end = td->body_end_offset - 1;
                while (body_content_start < body_content_end &&
                       isspace((unsigned char)source[body_content_start])) {
                    body_content_start++;
                }
                while (body_content_end > body_content_start &&
                       isspace((unsigned char)source[body_content_end - 1])) {
                    body_content_end--;
                }
                if (body_content_start < body_content_end) {
                    transpiler_copy_range(out, source, body_content_start, body_content_end);
                }
                fprintf(out, "\n}\n");
            }
            /* Emit cluster and handle declarations */
            fprintf(out, "\nthreads_cluster_t %s = {0};\n", block.cluster_name);
            for (size_t t = 0; t < block.thread_count; ++t) {
                fprintf(out, "threads_thread_handle_t %s_%s_handle = {0};\n",
                        block.cluster_name, block.threads[t].name);
            }
            fprintf(out, "threads_thread_handle_t *%s__handles[] = {\n", block.cluster_name);
            for (size_t t = 0; t < block.thread_count; ++t) {
                fprintf(out, "    &%s_%s_handle,\n", block.cluster_name, block.threads[t].name);
            }
            fprintf(out, "};\n");
            fprintf(out, "size_t %s__handle_count = %zu;\n", block.cluster_name, block.thread_count);
            /* Replace original block with a comment */
            fprintf(out, "/* threads '%s' lowered */", block.cluster_name);
            last_emit = block.end_offset;
            i = block.next_index ? block.next_index - 1 : i;
            parser_free_threads_block(&block);
            continue;
        }
        /* threads.cluster.thread.init(arg) → threads_thread_start(...) */
        if (tok->kind == TOKEN_IDENTIFIER && i + 5 < tokens.count) {
            if (token_is_symbol(&tokens.items[i + 1], '.') &&
                tokens.items[i + 2].kind == TOKEN_IDENTIFIER &&
                token_is_symbol(&tokens.items[i + 3], '.') &&
                token_is_identifier(&tokens.items[i + 4], "init") &&
                token_is_symbol(&tokens.items[i + 5], '(')) {
                size_t paren_idx = i + 5;
                int depth = 0;
                size_t closing_idx = SIZE_MAX;
                for (size_t j = paren_idx; j < tokens.count; ++j) {
                    if (token_is_symbol(&tokens.items[j], '(')) {
                        depth++;
                    } else if (token_is_symbol(&tokens.items[j], ')')) {
                        depth--;
                        if (depth == 0) {
                            closing_idx = j;
                            break;
                        }
                    }
                }
                if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                    ok = false;
                    break;
                }
                size_t cluster_len = tok->length;
                size_t thread_len = tokens.items[i + 2].length;
                char cluster_name[64] = {0};
                char thread_name[64] = {0};
                if (cluster_len >= sizeof(cluster_name)) cluster_len = sizeof(cluster_name) - 1;
                if (thread_len >= sizeof(thread_name)) thread_len = sizeof(thread_name) - 1;
                memcpy(cluster_name, tok->lexeme, cluster_len);
                memcpy(thread_name, tokens.items[i + 2].lexeme, thread_len);
                size_t arg_start = tokens.items[paren_idx].offset + 1;
                size_t arg_end = (closing_idx < tokens.count) ? tokens.items[closing_idx].offset : arg_start;
                fprintf(out, "threads_thread_start(&%s, %s_%s_body, NULL, NULL, 0, &%s_%s_handle)",
                        cluster_name, cluster_name, thread_name, cluster_name, thread_name);
                last_emit = (closing_idx < tokens.count)
                                ? tokens.items[closing_idx].offset + tokens.items[closing_idx].length
                                : tokens.items[paren_idx].offset + 1;
                i = (closing_idx < tokens.count) ? closing_idx : i + 5;
                continue;
            }
        }
        /* cluster.join_all() → threads_cluster_join_all(&cluster) */
        if (tok->kind == TOKEN_IDENTIFIER && i + 3 < tokens.count) {
            if (token_is_symbol(&tokens.items[i + 1], '.') &&
                token_is_identifier(&tokens.items[i + 2], "join_all") &&
                token_is_symbol(&tokens.items[i + 3], '(')) {
                size_t paren_idx = i + 3;
                int depth = 0;
                size_t closing_idx = SIZE_MAX;
                for (size_t j = paren_idx; j < tokens.count; ++j) {
                    if (token_is_symbol(&tokens.items[j], '(')) {
                        depth++;
                    } else if (token_is_symbol(&tokens.items[j], ')')) {
                        depth--;
                        if (depth == 0) {
                            closing_idx = j;
                            break;
                        }
                    }
                }
                if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                    ok = false;
                    break;
                }
                size_t cluster_len = tok->length;
                char cluster_name[64] = {0};
                if (cluster_len >= sizeof(cluster_name)) cluster_len = sizeof(cluster_name) - 1;
                memcpy(cluster_name, tok->lexeme, cluster_len);
                fprintf(out, "for (size_t _margo_t = 0; _margo_t < %s__handle_count; ++_margo_t) {\n",
                        cluster_name);
                fprintf(out, "    threads_thread_join(%s__handles[_margo_t]);\n",
                        cluster_name);
                fprintf(out, "}\n");
                last_emit = (closing_idx < tokens.count)
                                ? tokens.items[closing_idx].offset + tokens.items[closing_idx].length
                                : tokens.items[paren_idx].offset + 1;
                i = (closing_idx < tokens.count) ? closing_idx : i + 3;
                continue;
            }
        }
        if (token_is_identifier(tok, "switch")) {
            size_t after_switch = tok->offset + tok->length;
            while (after_switch < source_len && isspace((unsigned char)source[after_switch])) {
                after_switch++;
            }
            if (after_switch >= source_len || source[after_switch] != '(') {
                switch_header_t header;
                if (!parser_parse_switch_header(source, &tokens, i, &header, diag)) {
                    ok = false;
                    break;
                }
                if (!transpiler_copy_range(out, source, last_emit, header.start_offset)) {
                    diagnostic_set(diag, 0, "failed to copy prefix before switch");
                    parser_free_switch_header(&header);
                    ok = false;
                    break;
                }
                if (fprintf(out, "switch (%s)", header.condition) < 0) {
                    diagnostic_set(diag, 0, "failed to emit rewritten switch");
                    parser_free_switch_header(&header);
                    ok = false;
                    break;
                }
                if (!transpiler_copy_range(out, source, header.condition_end_offset, header.body_offset)) {
                    diagnostic_set(diag, 0, "failed to copy spacing after switch condition");
                    parser_free_switch_header(&header);
                    ok = false;
                    break;
                }
                last_emit = header.body_offset;
                i = header.next_index ? header.next_index - 1 : i;
                parser_free_switch_header(&header);
                continue;
            }
        }
        if (token_is_identifier(tok, "type")) {
            type_alias_t alias;
            if (!parser_parse_type_alias(source, &tokens, i, &alias, diag)) {
                ok = false;
                break;
            }
            if (!transpiler_copy_range(out, source, last_emit, alias.start_offset)) {
                diagnostic_set(diag, 0, "failed to copy prefix before type alias");
                parser_free_type_alias(&alias);
                ok = false;
                break;
            }
            if (fprintf(out, "typedef %s %s;", alias.underlying, alias.alias) < 0) {
                diagnostic_set(diag, 0, "failed to emit type alias");
                parser_free_type_alias(&alias);
                ok = false;
                break;
            }
            last_emit = alias.end_offset;
            i = alias.next_index ? alias.next_index - 1 : i;
            parser_free_type_alias(&alias);
            continue;
        }
        if (token_is_identifier(tok, "defer")) {
            defer_stmt_t defer;
            if (!parser_parse_defer_stmt(source, &tokens, i, &defer, diag)) {
                ok = false;
                break;
            }
            if (!transpiler_copy_range(out, source, last_emit, defer.start_offset)) {
                diagnostic_set(diag, 0, "failed to copy prefix before defer");
                parser_free_defer_stmt(&defer);
                ok = false;
                break;
            }
            /* Don't emit anything now; register in defer stack */
            if (!defer_push(&defer_stack, defer.expression, brace_depth)) {
                diagnostic_set(diag, 0, "out of memory while registering defer");
                parser_free_defer_stmt(&defer);
                ok = false;
                break;
            }
            last_emit = defer.end_offset;
            i = defer.next_index ? defer.next_index - 1 : i;
            parser_free_defer_stmt(&defer);
            continue;
        }
        if (token_is_identifier(tok, "while")) {
            size_t after_while = tok->offset + tok->length;
            while (after_while < source_len && isspace((unsigned char)source[after_while])) {
                after_while++;
            }
            if (after_while >= source_len || source[after_while] != '(') {
                while_header_t header;
                if (!parser_parse_while_header(source, &tokens, i, &header, diag)) {
                    ok = false;
                    break;
                }
                if (!transpiler_copy_range(out, source, last_emit, header.start_offset)) {
                    diagnostic_set(diag, 0, "failed to copy prefix before while");
                    parser_free_while_header(&header);
                    ok = false;
                    break;
                }
                if (fprintf(out, "while (%s)", header.condition) < 0) {
                    diagnostic_set(diag, 0, "failed to emit rewritten while");
                    parser_free_while_header(&header);
                    ok = false;
                    break;
                }
                if (!transpiler_copy_range(out, source, header.condition_end_offset, header.body_offset)) {
                    diagnostic_set(diag, 0, "failed to copy spacing after while condition");
                    parser_free_while_header(&header);
                    ok = false;
                    break;
                }
                last_emit = header.body_offset;
                i = header.next_index ? header.next_index - 1 : i;
                parser_free_while_header(&header);
                continue;
            }
        }
        if (token_is_identifier(tok, "if")) {
            size_t after_if = tok->offset + tok->length;
            while (after_if < source_len && isspace((unsigned char)source[after_if])) {
                after_if++;
            }
            if (after_if >= source_len || source[after_if] != '(') {
                if_header_t header;
                if (!parser_parse_if_header(source, &tokens, i, &header, diag)) {
                    ok = false;
                    break;
                }
                if (!transpiler_copy_range(out, source, last_emit, header.start_offset)) {
                    diagnostic_set(diag, 0, "failed to copy prefix before if");
                    parser_free_if_header(&header);
                    ok = false;
                    break;
                }
                if (fprintf(out, "if (%s)", header.condition) < 0) {
                    diagnostic_set(diag, 0, "failed to emit rewritten if");
                    parser_free_if_header(&header);
                    ok = false;
                    break;
                }
                /* Blocked if: a condition terminated by a newline opens a
                 * brace body that runs until the matching else/endif. */
                if (header.next_index < tokens.count &&
                    tokens.items[header.next_index].kind == TOKEN_NEWLINE) {
                    const token_t *nl = &tokens.items[header.next_index];
                    if (fputs(" {", out) == EOF) {
                        diagnostic_set(diag, 0, "failed to open blocked if body");
                        parser_free_if_header(&header);
                        ok = false;
                        break;
                    }
                    if (!transpiler_copy_range(out, source, header.condition_end_offset,
                                               nl->offset + nl->length)) {
                        diagnostic_set(diag, 0, "failed to copy spacing after if condition");
                        parser_free_if_header(&header);
                        ok = false;
                        break;
                    }
                    /* The branch body counts as a real scope so RAII frees
                     * land inside the virtual braces. */
                    if (!blocked_if_stack_push(&blocked_ifs, brace_depth + 1)) {
                        diagnostic_set(diag, tok->line, "out of memory while tracking blocked if");
                        parser_free_if_header(&header);
                        ok = false;
                        break;
                    }
                    brace_depth++;
                    last_emit = nl->offset + nl->length;
                    i = header.next_index;
                    parser_free_if_header(&header);
                    continue;
                }
                if (!transpiler_copy_range(out, source, header.condition_end_offset, header.body_offset)) {
                    diagnostic_set(diag, 0, "failed to copy spacing after if condition");
                    parser_free_if_header(&header);
                    ok = false;
                    break;
                }
                last_emit = header.body_offset;
                i = header.next_index ? header.next_index - 1 : i;
                parser_free_if_header(&header);
                continue;
            }
        }
        /* `else` / `endif` closing a blocked if (line-start tokens only, and
         * only at the brace depth of the branch body). */
        if (token_is_identifier(tok, "else") &&
            blocked_if_stack_matches(&blocked_ifs, brace_depth) &&
            (i == 0 || tokens.items[i - 1].kind == TOKEN_NEWLINE)) {
            size_t j = i + 1;
            if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                diagnostic_set(diag, 0, "failed to copy prefix before else");
                ok = false;
                break;
            }
            last_emit = tok->offset;
            /* Close the branch scope like a real '}': frees + defers first. */
            raii_live_emit_scope_frees(&raii_live, brace_depth, out);
            defer_emit_scope(&defer_stack, brace_depth, out);
            brace_depth--;
            if (j < tokens.count && tokens.items[j].kind != TOKEN_NEWLINE &&
                token_is_identifier(&tokens.items[j], "if")) {
                if_header_t header;
                if (!parser_parse_if_header(source, &tokens, j, &header, diag)) {
                    ok = false;
                    break;
                }
                if (fprintf(out, "} else if (%s)", header.condition) < 0) {
                    diagnostic_set(diag, 0, "failed to emit rewritten else if");
                    parser_free_if_header(&header);
                    ok = false;
                    break;
                }
                if (header.next_index < tokens.count &&
                    tokens.items[header.next_index].kind == TOKEN_NEWLINE) {
                    const token_t *nl = &tokens.items[header.next_index];
                    if (fputs(" {", out) == EOF ||
                        !transpiler_copy_range(out, source, header.condition_end_offset,
                                               nl->offset + nl->length)) {
                        diagnostic_set(diag, 0, "failed to emit blocked else-if body");
                        parser_free_if_header(&header);
                        ok = false;
                        break;
                    }
                    brace_depth++;
                    last_emit = nl->offset + nl->length;
                    i = header.next_index;
                } else {
                    /* `else if cond {` or same-line statement: the chain
                     * continues as plain C, so the frame is consumed. */
                    if (!transpiler_copy_range(out, source, header.condition_end_offset,
                                               header.body_offset)) {
                        diagnostic_set(diag, 0, "failed to copy spacing after else-if condition");
                        parser_free_if_header(&header);
                        ok = false;
                        break;
                    }
                    last_emit = header.body_offset;
                    i = header.next_index ? header.next_index - 1 : i;
                    blocked_ifs.count--;
                }
                parser_free_if_header(&header);
                continue;
            }
            if (fputs("} else", out) == EOF) {
                diagnostic_set(diag, 0, "failed to emit blocked else");
                ok = false;
                break;
            }
            if (j < tokens.count && tokens.items[j].kind == TOKEN_NEWLINE) {
                const token_t *nl = &tokens.items[j];
                if (fputs(" {", out) == EOF ||
                    !transpiler_copy_range(out, source, tok->offset + tok->length,
                                           nl->offset + nl->length)) {
                    diagnostic_set(diag, 0, "failed to emit blocked else body");
                    ok = false;
                    break;
                }
                brace_depth++;
                last_emit = nl->offset + nl->length;
                i = j;
                continue;
            }
            /* `else {` or same-line statement: plain C takes over. */
            blocked_ifs.count--;
            last_emit = tok->offset + tok->length;
            continue;
        }
        if (token_is_identifier(tok, "endif") &&
            blocked_if_stack_matches(&blocked_ifs, brace_depth) &&
            (i == 0 || tokens.items[i - 1].kind == TOKEN_NEWLINE)) {
            if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                diagnostic_set(diag, 0, "failed to copy prefix before endif");
                ok = false;
                break;
            }
            last_emit = tok->offset;
            /* Close the branch scope like a real '}': frees + defers first. */
            raii_live_emit_scope_frees(&raii_live, brace_depth, out);
            defer_emit_scope(&defer_stack, brace_depth, out);
            brace_depth--;
            if (fputs("}", out) == EOF) {
                diagnostic_set(diag, 0, "failed to close blocked if body");
                ok = false;
                break;
            }
            last_emit = tok->offset + tok->length;
            blocked_ifs.count--;
            continue;
        }

        /* `return` – inject frees for all owned vars before the keyword */
        if (token_is_identifier(tok, "return") && fn_body_depth > 0) {
            char *ret_ident = extract_return_ident(&tokens, i);
            if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                diagnostic_set(diag, tok->line, "failed to copy prefix before return");
                free(ret_ident);
                ok = false;
                break;
            }
            last_emit = tok->offset;
            raii_live_emit_return_frees(&raii_live,
                                        fn_body_depth, brace_depth,
                                        ret_ident, out);
            defer_emit_return(&defer_stack, fn_body_depth, brace_depth, out);
            free(ret_ident);
            /* Fall through to emit `return` normally */
        }

        /* RAII: look for alloc assignment at this identifier position */
        if (tok->kind == TOKEN_IDENTIFIER) {
            try_register_alloc_ownership(&tokens, i, brace_depth, &raii_live);
        }

        if (token_is_symbol(tok, '{')) {
            size_t before_last_emit = last_emit;
            if (!style_handle_open_brace(&style_blocks, source, out, tok, &last_emit, diag)) {
                ok = false;
                break;
            }
            if (last_emit == before_last_emit) {
                /* Emit the brace itself if style_handle_open_brace didn't */
                if (!transpiler_copy_range(out, source, last_emit, tok->offset + tok->length)) {
                    diagnostic_set(diag, tok->line, "failed to copy '{'");
                    ok = false;
                    break;
                }
                last_emit = tok->offset + tok->length;
            }
            brace_depth++;
            if (pending_for_in_array) {
                if (fprintf(out, " auto %s = %s[_margo_i]; ", pending_for_in_var, pending_for_in_iterable) < 0) {
                    ok = false;
                    break;
                }
                pending_for_in_array = false;
            }
            /* Record depth of the first brace after any `fn` keyword so we
             * know where function scope begins for return-path RAII. */
            if (fn_body_depth == 0 && brace_depth > 0) {
                fn_body_depth = brace_depth;
            }
            continue;
        }
        if (token_is_symbol(tok, '}')) {
            if (brace_depth <= 0) {
                diagnostic_set(diag, tok->line, "unbalanced closing brace");
                ok = false;
                break;
            }
            /* A real closing brace at the frame depth would swallow the
             * enclosing scope of an unterminated blocked if. */
            if (blocked_if_stack_matches(&blocked_ifs, brace_depth)) {
                diagnostic_set(diag, tok->line, "if block is missing endif");
                ok = false;
                break;
            }
            /* RAII: inject frees before closing this scope */
            if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                diagnostic_set(diag, tok->line, "failed to copy prefix before '}'");
                ok = false;
                break;
            }
            last_emit = tok->offset;
            raii_live_emit_scope_frees(&raii_live, brace_depth, out);
            defer_emit_scope(&defer_stack, brace_depth, out);

            if (!style_handle_close_brace(&style_blocks, source, out, tok, &last_emit, brace_depth, diag)) {
                ok = false;
                break;
            }
            /* When closing a function body, reset fn_body_depth so the next
             * function that opens a brace records itself properly. */
            if (brace_depth == fn_body_depth) {
                fn_body_depth = 0;
            }
            brace_depth--;
            continue;
        }
    }
    if (ok && blocked_ifs.count > 0) {
        diagnostic_set(diag, 0, "if block is missing endif");
        ok = false;
    }
    if (ok && std_io_used && !std_io_imported) {
        diagnostic_set(diag,
                       std_io_use_line ? std_io_use_line : 0,
                       "Scan/ScanLine/in require '@import std/io'");
        ok = false;
    }
    if (ok) {
        if (!transpiler_copy_range(out, source, last_emit, source_len)) {
            diagnostic_set(diag, 0, "failed to write tail of source");
            ok = false;
        }
    }
    if (ok && has_start_function && !has_main_function) {
        if (fprintf(out,
                    "\nint main(int argc, char **argv) {\n"
                    "    return start(argc, argv);\n"
                    "}\n") < 0) {
            diagnostic_set(diag, 0, "failed to emit main wrapper");
            ok = false;
        }
    }
    if (fclose(out) != 0) {
        diagnostic_set(diag, 0, "failed to finalize transpilation output");
        ok = false;
    }
    if (ok) {
        if (!insert_implicit_semicolons(&generated, &generated_size, diag)) {
            ok = false;
        }
    }
    raii_live_free(&raii_live);
    defer_free(&defer_stack);
    free(pending_for_in_iterable);
    style_stack_free(&style_blocks);
    blocked_if_stack_free(&blocked_ifs);
    lexer_free(&tokens);
    free(source);
    if (!ok) {
        free(generated);
        return false;
    }
    *buffer_out = generated;
    *size_out = generated_size;
    return true;
}
