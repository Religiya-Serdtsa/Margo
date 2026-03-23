#define _GNU_SOURCE
#include "transpiler.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        if (fprintf(out, "ttak_mem_free(%s);\n", v->name) < 0) {
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
        if (fprintf(out, "ttak_mem_free(%s);\n", v->name) < 0) {
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

/**
 * @brief Check whether token at *index* is `IDENT = alloc(` and register
 *        the variable as owned if so.
 *
 * Advances nothing — this is a pure lookahead that does not move *index*.
 */
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
    bool is_alloc =
        (strncmp(rhs->lexeme, "alloc_and_init", rhs->length) == 0 && rhs->length == 14) ||
        (strncmp(rhs->lexeme, "alloc",         rhs->length) == 0 && rhs->length == 5);
    if (!is_alloc) {
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
    static const char *lines[] = {
        "#include <stdbool.h>\n",
        "#include <stddef.h>\n",
        "#include <stdint.h>\n",
        "#include <string.h>\n",
        "#include <stdio.h>\n",
        "#include <ttak/mem/mem.h>\n",
        "#include <ttak/mem/epoch_gc.h>\n",
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
        "    size_t *: margo_scan_param_size_t, \\\n",
        "    float *: margo_scan_param_float, \\\n",
        "    double *: margo_scan_param_double, \\\n",
        "    long double *: margo_scan_param_long_double, \\\n",
        "    char *: margo_scan_param_string, \\\n",
        "    string: margo_scan_param_const_string, \\\n",
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
        "static inline void *margo_builtin_alloc_bytes(size_t bytes) {\n",
        "    return ttak_mem_alloc(bytes, __TTAK_UNSAFE_MEM_FOREVER__, margo_now_ticks());\n",
        "}\n",
        "static inline void *margo_builtin_alloc_and_copy(size_t bytes, const void *src, size_t src_len) {\n",
        "    void *dst = margo_builtin_alloc_bytes(bytes);\n",
        "    if (dst && src && src_len) {\n",
        "        size_t copy = src_len < bytes ? src_len : bytes;\n",
        "        memcpy(dst, src, copy);\n",
        "    }\n",
        "    return dst;\n",
        "}\n",
        "#define alloc(size) margo_builtin_alloc_bytes((size_t)(size))\n",
        "#define alloc_and_init(size, literal) margo_builtin_alloc_and_copy((size_t)(size), (literal), sizeof(literal))\n",
        NULL,
    };
    for (size_t i = 0; lines[i]; ++i) {
        fputs(lines[i], out);
    }
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
            return false;
        default:
            return true;
    }
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
    char prev_char = '\0';
    bool line_has_assignment = false;
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        char next = (i + 1 < len) ? src[i + 1] : '\0';
        if (c == '\r' || c == '\n') {
            if (line_is_directive && has_pending_user_state) {
                in_user_section = pending_user_state;
            } else if (!line_is_directive && in_user_section && !in_line_comment && !in_block_comment) {
                bool need_semicolon = should_insert_semicolon(last_non_ws);
                if (!need_semicolon && last_non_ws == '}' && line_has_assignment) {
                    need_semicolon = true;
                }
                if (need_semicolon) {
                    out[w++] = ';';
                }
            }
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
            line_has_assignment = false;
            prev_char = '\0';
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
            last_non_ws = c;
        }
        if (!in_line_comment && !in_block_comment && !in_string && !in_char_literal) {
            if (c == '=' && next != '=' && prev_char != '<' && prev_char != '>' &&
                prev_char != '!' && prev_char != '=') {
                line_has_assignment = true;
            }
        }
        if (!in_line_comment && !in_block_comment && !part_of_comment_delim && !isspace((unsigned char)c)) {
            prev_char = c;
        }
    }
    if (in_user_section && !in_line_comment && !in_block_comment && should_insert_semicolon(last_non_ws)) {
        out[w++] = ';';
    }
    out[w] = '\0';
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
        bool has_suffix = len >= 2 && strcmp(name + len - 2, ".h") == 0;
        const char *fmt = has_suffix ? "#include <%s>\n" : "#include <%s.h>\n";
        if (fprintf(out, fmt, name) < 0) {
            diagnostic_set(diag, 0, "failed to emit include for %s", name);
            return false;
        }
        return true;
    }
    if (dir->kind == IMPORT_KIND_CPP) {
        /* C++ binding support is a future milestone.  Preserve as comment so
         * the source remains parseable and the intent is documented. */
        if (fprintf(out, "/* @import c++/%s (C++ bindings: future milestone) */\n",
                    dir->target ? dir->target : "") < 0) {
            diagnostic_set(diag, 0, "failed to emit c++ import stub");
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
    if (!read_file(input_path, &source, &source_len, diag)) {
        return false;
    }
    token_buffer_t tokens;
    if (!lexer_tokenize(source, source_len, &tokens, diag)) {
        free(source);
        return false;
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
    bool has_start_function = false;
    bool has_main_function = false;
    bool std_io_imported = false;
    bool std_io_used = false;
    size_t std_io_use_line = 0;
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
            if (!handle_scan_call(out, source, &tokens, &i, &last_emit, require_newline, &std_io_used, &std_io_use_line, diag)) {
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
            for_header_t header;
            if (!parser_parse_for_header(source, &tokens, i, &header, diag)) {
                ok = false;
                break;
            }
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
            free(ret_ident);
            /* Fall through to emit `return` normally */
        }

        /* RAII: look for alloc assignment at this identifier position */
        if (tok->kind == TOKEN_IDENTIFIER) {
            try_register_alloc_ownership(&tokens, i, brace_depth, &raii_live);
        }

        if (token_is_symbol(tok, '{')) {
            if (!style_handle_open_brace(&style_blocks, source, out, tok, &last_emit, diag)) {
                ok = false;
                break;
            }
            brace_depth++;
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
            /* RAII: inject frees before closing this scope */
            if (!transpiler_copy_range(out, source, last_emit, tok->offset)) {
                diagnostic_set(diag, tok->line, "failed to copy prefix before '}'");
                ok = false;
                break;
            }
            last_emit = tok->offset;
            raii_live_emit_scope_frees(&raii_live, brace_depth, out);

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
    if (ok && std_io_used && !std_io_imported) {
        diagnostic_set(diag,
                       std_io_use_line ? std_io_use_line : 0,
                       "Scan/ScanLine require '@import std/io'");
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
    style_stack_free(&style_blocks);
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
