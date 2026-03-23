#include "sema.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file sema.c
 * @brief Semantic analysis pass for Margo.
 *
 * The pass performs four distinct jobs over the token stream:
 *
 * 1. **Function signature registry** – every `fn` declaration is recorded
 *    so that later call sites can be checked for arity mismatches.
 *
 * 2. **Symbol table with scope tracking** – variable declarations are
 *    recorded per brace-depth scope so that uses of undeclared names can be
 *    reported without stopping compilation.
 *
 * 3. **RAII ownership table** – every assignment of the form
 *    `name = alloc(...)` / `name = alloc_and_init(...)` is recorded with
 *    the scope depth at which the variable lives.  The transpiler uses this
 *    table to inject `free()` calls before scope-closing braces and before
 *    `return` statements.
 *
 * 4. **`weird` dimension tracking** – `weird(T, N)` sites record the
 *    expected pointer depth so that usage in dereferences and assignments
 *    can be cross-checked at the semantic level.
 *
 * The analysis is deliberately permissive: it emits non-fatal warnings
 * rather than hard errors whenever the information is insufficient for a
 * definitive judgement (e.g. types from C headers are opaque).  Fatal
 * errors are reserved for conditions that prevent building the RAII table
 * (OOM).
 */

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

static char *sema_dup(const char *src) {
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, src, len + 1);
    }
    return copy;
}

static char *sema_dup_range(const char *src, size_t start, size_t end) {
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

/* =========================================================================
 * Type kind table
 * ====================================================================== */

/**
 * @brief Map a well-known Margo / C identifier to its type kind.
 *
 * Returns MARGO_TYPE_UNKNOWN when the identifier is not a built-in type.
 * Struct/union/enum tags are handled separately via the STRUCT/UNION/ENUM
 * path that checks the keyword preceding the tag.
 */
static margo_type_kind_t keyword_to_type_kind(const char *name, size_t len) {
    struct { const char *kw; margo_type_kind_t kind; } table[] = {
        { "void",              MARGO_TYPE_VOID     },
        { "int",               MARGO_TYPE_INT      },
        { "long",              MARGO_TYPE_LONG     },
        { "float",             MARGO_TYPE_FLOAT    },
        { "double",            MARGO_TYPE_DOUBLE   },
        { "char",              MARGO_TYPE_CHAR     },
        { "short",             MARGO_TYPE_SHORT    },
        { "bool",              MARGO_TYPE_BOOL     },
        { "string",            MARGO_TYPE_STRING   },
        { "auto",              MARGO_TYPE_AUTO     },
        { "size_t",            MARGO_TYPE_SIZE_T   },
        { "uint8_t",           MARGO_TYPE_UINT8    },
        { "uint16_t",          MARGO_TYPE_UINT16   },
        { "uint32_t",          MARGO_TYPE_UINT32   },
        { "uint64_t",          MARGO_TYPE_UINT64   },
        { "int8_t",            MARGO_TYPE_INT8     },
        { "int16_t",           MARGO_TYPE_INT16    },
        { "int32_t",           MARGO_TYPE_INT32    },
        { "int64_t",           MARGO_TYPE_INT64    },
        { "unsigned",          MARGO_TYPE_UINT     },
        { "signed",            MARGO_TYPE_INT      },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        size_t klen = strlen(table[i].kw);
        if (klen == len && strncmp(table[i].kw, name, len) == 0) {
            return table[i].kind;
        }
    }
    return MARGO_TYPE_UNKNOWN;
}

const char *sema_type_kind_name(margo_type_kind_t kind) {
    switch (kind) {
        case MARGO_TYPE_UNKNOWN:   return "unknown";
        case MARGO_TYPE_VOID:      return "void";
        case MARGO_TYPE_INT:       return "int";
        case MARGO_TYPE_LONG:      return "long";
        case MARGO_TYPE_LONGLONG:  return "long long";
        case MARGO_TYPE_UINT:      return "unsigned int";
        case MARGO_TYPE_ULONG:     return "unsigned long";
        case MARGO_TYPE_ULONGLONG: return "unsigned long long";
        case MARGO_TYPE_CHAR:      return "char";
        case MARGO_TYPE_UCHAR:     return "unsigned char";
        case MARGO_TYPE_SHORT:     return "short";
        case MARGO_TYPE_USHORT:    return "unsigned short";
        case MARGO_TYPE_FLOAT:     return "float";
        case MARGO_TYPE_DOUBLE:    return "double";
        case MARGO_TYPE_LDOUBLE:   return "long double";
        case MARGO_TYPE_BOOL:      return "bool";
        case MARGO_TYPE_STRING:    return "string";
        case MARGO_TYPE_AUTO:      return "auto";
        case MARGO_TYPE_WEIRD:     return "weird";
        case MARGO_TYPE_STRUCT:    return "struct";
        case MARGO_TYPE_UNION:     return "union";
        case MARGO_TYPE_ENUM:      return "enum";
        case MARGO_TYPE_FUNC:      return "fn";
        case MARGO_TYPE_SIZE_T:    return "size_t";
        case MARGO_TYPE_UINT8:     return "uint8_t";
        case MARGO_TYPE_UINT16:    return "uint16_t";
        case MARGO_TYPE_UINT32:    return "uint32_t";
        case MARGO_TYPE_UINT64:    return "uint64_t";
        case MARGO_TYPE_INT8:      return "int8_t";
        case MARGO_TYPE_INT16:     return "int16_t";
        case MARGO_TYPE_INT32:     return "int32_t";
        case MARGO_TYPE_INT64:     return "int64_t";
    }
    return "?";
}

void sema_type_free(margo_type_t *t) {
    if (!t) {
        return;
    }
    free(t->name);
    t->name = NULL;
}

/* =========================================================================
 * Type parser
 * ====================================================================== */

/**
 * @brief Skip newlines and return the index of the next substantive token.
 */
static size_t skip_nl(const token_buffer_t *tokens, size_t i) {
    while (i < tokens->count && tokens->items[i].kind == TOKEN_NEWLINE) {
        i++;
    }
    return i;
}

bool sema_parse_type(const char *source,
                     const token_buffer_t *tokens,
                     size_t *index,
                     margo_type_t *out) {
    if (!tokens || !index || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    size_t i = skip_nl(tokens, *index);
    if (i >= tokens->count) {
        return false;
    }
    const token_t *tok = &tokens->items[i];

    /* weird(T, N) */
    if (tok->kind == TOKEN_IDENTIFIER &&
        strncmp(tok->lexeme, "weird", tok->length) == 0 && tok->length == 5) {
        size_t j = skip_nl(tokens, i + 1);
        if (j >= tokens->count || !token_is_symbol(&tokens->items[j], '(')) {
            return false;
        }
        j = skip_nl(tokens, j + 1);
        if (j >= tokens->count) {
            return false;
        }
        /* base type */
        size_t type_start = j;
        margo_type_t base = {0};
        if (!sema_parse_type(source, tokens, &type_start, &base)) {
            /* Allow opaque external identifiers inside weird(T, N), e.g. FILE. */
            const token_t *base_tok = &tokens->items[j];
            if (base_tok->kind != TOKEN_IDENTIFIER) {
                return false;
            }
            base.kind = MARGO_TYPE_UNKNOWN;
            base.name = sema_dup_range(source,
                                       base_tok->offset,
                                       base_tok->offset + base_tok->length);
            if (!base.name) {
                return false;
            }
            type_start = j + 1;
        }
        j = skip_nl(tokens, type_start);
        if (j >= tokens->count || !token_is_symbol(&tokens->items[j], ',')) {
            sema_type_free(&base);
            return false;
        }
        j = skip_nl(tokens, j + 1);
        if (j >= tokens->count || tokens->items[j].kind != TOKEN_NUMBER) {
            sema_type_free(&base);
            return false;
        }
        int depth = (int)strtol(tokens->items[j].lexeme, NULL, 10);
        j = skip_nl(tokens, j + 1);
        if (j >= tokens->count || !token_is_symbol(&tokens->items[j], ')')) {
            sema_type_free(&base);
            return false;
        }
        out->kind      = MARGO_TYPE_WEIRD;
        out->base_kind = base.kind;
        out->ptr_depth = depth;
        out->name      = base.name ? base.name : NULL;
        base.name      = NULL;
        sema_type_free(&base);
        *index = j + 1;
        return true;
    }

    /* struct / union / enum tag */
    if (tok->kind == TOKEN_IDENTIFIER &&
        ((strncmp(tok->lexeme, "struct", tok->length) == 0 && tok->length == 6) ||
         (strncmp(tok->lexeme, "union",  tok->length) == 0 && tok->length == 5) ||
         (strncmp(tok->lexeme, "enum",   tok->length) == 0 && tok->length == 4))) {
        margo_type_kind_t compound_kind =
            (tok->length == 6) ? MARGO_TYPE_STRUCT :
            (tok->length == 5) ? MARGO_TYPE_UNION  : MARGO_TYPE_ENUM;
        size_t j = skip_nl(tokens, i + 1);
        if (j < tokens->count && tokens->items[j].kind == TOKEN_IDENTIFIER) {
            out->kind = compound_kind;
            out->name = sema_dup_range(source,
                                       tokens->items[j].offset,
                                       tokens->items[j].offset + tokens->items[j].length);
            *index = j + 1;
        } else {
            out->kind = compound_kind;
            *index = i + 1;
        }
        return true;
    }

    /* const qualifier: consume and recurse */
    if (tok->kind == TOKEN_IDENTIFIER &&
        strncmp(tok->lexeme, "const", tok->length) == 0 && tok->length == 5) {
        *index = i + 1;
        bool ok = sema_parse_type(source, tokens, index, out);
        if (ok) {
            out->is_const = true;
        }
        return ok;
    }

    /* unsigned / signed modifier */
    if (tok->kind == TOKEN_IDENTIFIER &&
        ((strncmp(tok->lexeme, "unsigned", tok->length) == 0 && tok->length == 8) ||
         (strncmp(tok->lexeme, "signed",   tok->length) == 0 && tok->length == 6))) {
        bool is_unsigned = (tok->length == 8);
        size_t j = skip_nl(tokens, i + 1);
        if (j < tokens->count && tokens->items[j].kind == TOKEN_IDENTIFIER) {
            const token_t *next = &tokens->items[j];
            if (strncmp(next->lexeme, "char",  next->length) == 0) {
                out->kind = is_unsigned ? MARGO_TYPE_UCHAR : MARGO_TYPE_CHAR;
                *index = j + 1;
                return true;
            }
            if (strncmp(next->lexeme, "short", next->length) == 0) {
                out->kind = is_unsigned ? MARGO_TYPE_USHORT : MARGO_TYPE_SHORT;
                *index = j + 1;
                return true;
            }
            if (strncmp(next->lexeme, "long",  next->length) == 0) {
                size_t k = skip_nl(tokens, j + 1);
                if (k < tokens->count && tokens->items[k].kind == TOKEN_IDENTIFIER &&
                    strncmp(tokens->items[k].lexeme, "long", 4) == 0 && tokens->items[k].length == 4) {
                    out->kind = is_unsigned ? MARGO_TYPE_ULONGLONG : MARGO_TYPE_LONGLONG;
                    *index = k + 1;
                    return true;
                }
                out->kind = is_unsigned ? MARGO_TYPE_ULONG : MARGO_TYPE_LONG;
                *index = j + 1;
                return true;
            }
            if (strncmp(next->lexeme, "int", next->length) == 0) {
                out->kind = is_unsigned ? MARGO_TYPE_UINT : MARGO_TYPE_INT;
                *index = j + 1;
                return true;
            }
        }
        out->kind = is_unsigned ? MARGO_TYPE_UINT : MARGO_TYPE_INT;
        *index = i + 1;
        return true;
    }

    /* long long / long double / long */
    if (tok->kind == TOKEN_IDENTIFIER &&
        strncmp(tok->lexeme, "long", tok->length) == 0 && tok->length == 4) {
        size_t j = skip_nl(tokens, i + 1);
        if (j < tokens->count && tokens->items[j].kind == TOKEN_IDENTIFIER) {
            const token_t *next = &tokens->items[j];
            if (strncmp(next->lexeme, "long", next->length) == 0 && next->length == 4) {
                out->kind = MARGO_TYPE_LONGLONG;
                *index = j + 1;
                return true;
            }
            if (strncmp(next->lexeme, "double", next->length) == 0 && next->length == 6) {
                out->kind = MARGO_TYPE_LDOUBLE;
                *index = j + 1;
                return true;
            }
        }
        out->kind = MARGO_TYPE_LONG;
        *index = i + 1;
        return true;
    }

    /* simple keyword type */
    if (tok->kind == TOKEN_IDENTIFIER) {
        margo_type_kind_t k = keyword_to_type_kind(tok->lexeme, tok->length);
        if (k != MARGO_TYPE_UNKNOWN) {
            out->kind = k;
            *index = i + 1;
            return true;
        }
    }

    return false;
}

/* =========================================================================
 * Diagnostic helpers
 * ====================================================================== */

static bool sema_diag_push(sema_diag_list_t *list,
                            size_t line,
                            bool is_error,
                            const char *fmt, ...) {
    if (!list) {
        return true;
    }
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 16;
        sema_diag_entry_t *new_items = realloc(list->items, new_cap * sizeof(sema_diag_entry_t));
        if (!new_items) {
            return false;
        }
        list->items    = new_items;
        list->capacity = new_cap;
    }
    sema_diag_entry_t *entry = &list->items[list->count++];
    entry->line     = line;
    entry->is_error = is_error;
    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, sizeof(entry->message), fmt, args);
    va_end(args);
    if (is_error) {
        list->error_count++;
    } else {
        list->warning_count++;
    }
    return true;
}

void sema_diag_list_free(sema_diag_list_t *list) {
    if (!list) {
        return;
    }
    free(list->items);
    list->items         = NULL;
    list->count         = 0;
    list->capacity      = 0;
    list->error_count   = 0;
    list->warning_count = 0;
}

void sema_diag_list_print(const sema_diag_list_t *list, const char *source_path) {
    if (!list || !list->items) {
        return;
    }
    const char *path = source_path ? source_path : "<unknown>";
    for (size_t i = 0; i < list->count; ++i) {
        const sema_diag_entry_t *e = &list->items[i];
        if (e->line) {
            fprintf(stderr, "%s:%zu: %s: %s\n",
                    path, e->line,
                    e->is_error ? "error" : "warning",
                    e->message);
        } else {
            fprintf(stderr, "%s: %s: %s\n",
                    path,
                    e->is_error ? "error" : "warning",
                    e->message);
        }
    }
}

/* =========================================================================
 * RAII table helpers
 * ====================================================================== */

static bool raii_table_push(raii_table_t *table,
                             const char *name,
                             int scope_depth,
                             size_t decl_line) {
    if (!table || !name) {
        return false;
    }
    if (table->count == table->capacity) {
        size_t new_cap = table->capacity ? table->capacity * 2 : 16;
        raii_entry_t *new_items = realloc(table->items, new_cap * sizeof(raii_entry_t));
        if (!new_items) {
            return false;
        }
        table->items    = new_items;
        table->capacity = new_cap;
    }
    raii_entry_t *e = &table->items[table->count++];
    size_t name_len = strlen(name);
    if (name_len >= sizeof(e->name)) {
        name_len = sizeof(e->name) - 1;
    }
    memcpy(e->name, name, name_len);
    e->name[name_len] = '\0';
    e->scope_depth    = scope_depth;
    e->decl_line      = decl_line;
    e->transferred    = false;
    return true;
}

void raii_table_free(raii_table_t *table) {
    if (!table) {
        return;
    }
    free(table->items);
    table->items    = NULL;
    table->count    = 0;
    table->capacity = 0;
}

/* =========================================================================
 * Symbol table
 * ====================================================================== */

typedef struct {
    sema_symbol_t *items;
    size_t         count;
    size_t         capacity;
} sema_symtab_t;

static bool symtab_push(sema_symtab_t *tab,
                         const char *name,
                         margo_type_t type,
                         int depth,
                         bool is_param,
                         size_t decl_line) {
    if (!tab || !name) {
        return false;
    }
    if (tab->count == tab->capacity) {
        size_t new_cap = tab->capacity ? tab->capacity * 2 : 32;
        sema_symbol_t *new_items = realloc(tab->items, new_cap * sizeof(sema_symbol_t));
        if (!new_items) {
            return false;
        }
        tab->items    = new_items;
        tab->capacity = new_cap;
    }
    sema_symbol_t *sym = &tab->items[tab->count++];
    sym->name        = sema_dup(name);
    sym->type        = type;
    sym->scope_depth = depth;
    sym->is_param    = is_param;
    sym->decl_line   = decl_line;
    return sym->name != NULL;
}

/** Pop all symbols declared at the given depth. */
static void symtab_pop_depth(sema_symtab_t *tab, int depth) {
    if (!tab) {
        return;
    }
    while (tab->count > 0 && tab->items[tab->count - 1].scope_depth >= depth) {
        sema_symbol_t *sym = &tab->items[tab->count - 1];
        free(sym->name);
        sema_type_free(&sym->type);
        tab->count--;
    }
}

static sema_symbol_t *symtab_lookup(sema_symtab_t *tab, const char *name)
    __attribute__((unused));

static sema_symbol_t *symtab_lookup(sema_symtab_t *tab, const char *name) {
    if (!tab || !name) {
        return NULL;
    }
    for (size_t i = tab->count; i > 0; --i) {
        if (strcmp(tab->items[i - 1].name, name) == 0) {
            return &tab->items[i - 1];
        }
    }
    return NULL;
}

static void symtab_free(sema_symtab_t *tab) {
    if (!tab) {
        return;
    }
    for (size_t i = 0; i < tab->count; ++i) {
        free(tab->items[i].name);
        sema_type_free(&tab->items[i].type);
    }
    free(tab->items);
    tab->items    = NULL;
    tab->count    = 0;
    tab->capacity = 0;
}

/* =========================================================================
 * Function registry
 * ====================================================================== */

typedef struct {
    sema_func_t *items;
    size_t       count;
    size_t       capacity;
} sema_funcreg_t;

static bool funcreg_push(sema_funcreg_t *reg, sema_func_t fn) {
    if (!reg) {
        return false;
    }
    if (reg->count == reg->capacity) {
        size_t new_cap = reg->capacity ? reg->capacity * 2 : 16;
        sema_func_t *new_items = realloc(reg->items, new_cap * sizeof(sema_func_t));
        if (!new_items) {
            return false;
        }
        reg->items    = new_items;
        reg->capacity = new_cap;
    }
    reg->items[reg->count++] = fn;
    return true;
}

static sema_func_t *funcreg_lookup(sema_funcreg_t *reg, const char *name) {
    if (!reg || !name) {
        return NULL;
    }
    for (size_t i = 0; i < reg->count; ++i) {
        if (reg->items[i].name && strcmp(reg->items[i].name, name) == 0) {
            return &reg->items[i];
        }
    }
    return NULL;
}

static void sema_func_free(sema_func_t *fn) {
    if (!fn) {
        return;
    }
    free(fn->name);
    for (size_t i = 0; i < fn->param_count; ++i) {
        sema_type_free(&fn->param_types[i]);
        free(fn->param_names[i]);
    }
    free(fn->param_types);
    free(fn->param_names);
}

static void funcreg_free(sema_funcreg_t *reg) {
    if (!reg) {
        return;
    }
    for (size_t i = 0; i < reg->count; ++i) {
        sema_func_free(&reg->items[i]);
    }
    free(reg->items);
    reg->items    = NULL;
    reg->count    = 0;
    reg->capacity = 0;
}

/* =========================================================================
 * Shared analysis state
 * ====================================================================== */

typedef struct {
    const char           *source;
    const token_buffer_t *tokens;
    sema_symtab_t         symtab;
    sema_funcreg_t        funcreg;
    raii_table_t         *raii;
    sema_diag_list_t     *diags;
    int                   brace_depth;
    bool                  in_function;
    bool                  inside_style_block;
} sema_state_t;

/* =========================================================================
 * Forward-declaration scan for fn signatures (first pass)
 * ====================================================================== */

/**
 * @brief Parse a `fn` declaration from position fn_idx and register its
 *        signature into the function registry.
 *
 * We perform this in a first pass so that forward calls (functions called
 * before their declaration) are still validated.
 */
static bool collect_fn_signature(sema_state_t *st, size_t fn_idx) {
    /* fn [return_type] name ( [params] ) { ... }
     * fn name ( [params] ) { ... }           ← implicit int/void return
     * Decorators (@decorator) before fn are handled by the transpiler and
     * can appear between fn_idx-1 and the actual signature.  We skip any
     * @-prefixed tokens.
     */
    size_t i   = skip_nl(st->tokens, fn_idx + 1);
    margo_type_t return_type = { .kind = MARGO_TYPE_UNKNOWN };

    /* Collect tokens until we find the function name (identifier before '(').
     * Strategy: scan ahead for the first `IDENT (` pair where IDENT is not
     * `weird`, `fn`, a decorator name, or a type keyword that is immediately
     * followed by another type token.  Reuse sema_parse_type for the prefix. */
    bool decorator_next = false;
    while (i < st->tokens->count) {
        const token_t *tok = &st->tokens->items[i];
        if (tok->kind == TOKEN_EOF || tok->kind == TOKEN_NEWLINE) {
            i++;
            continue;
        }
        if (tok->kind == TOKEN_AT) {
            decorator_next = true;
            i++;
            continue;
        }
        if (decorator_next) {
            decorator_next = false;
            i++;
            continue;
        }
        /* Is this the function name? Check if followed by `(`. */
        if (tok->kind == TOKEN_IDENTIFIER) {
            size_t j = skip_nl(st->tokens, i + 1);
            if (j < st->tokens->count && token_is_symbol(&st->tokens->items[j], '(')) {
                /* Everything between fn_idx+1 and i is the return type. */
                if (return_type.kind == MARGO_TYPE_UNKNOWN) {
                    /* No explicit return type tokens seen; will be implicitly int. */
                    return_type.kind = MARGO_TYPE_INT;
                }
                /* Parse function name */
                char *fn_name = sema_dup_range(st->source, tok->offset, tok->offset + tok->length);
                if (!fn_name) {
                    return false;
                }
                /* Parse parameter list */
                size_t k = skip_nl(st->tokens, j + 1); /* first token inside '(' */
                sema_func_t fn;
                memset(&fn, 0, sizeof(fn));
                fn.name        = fn_name;
                fn.return_type = return_type;
                fn.decl_line   = tok->line;
                return_type.name = NULL; /* transferred */

                /* Accumulate params */
                size_t param_cap = 0;
                int depth_inside = 0;
                bool past_closing = false;
                while (k < st->tokens->count) {
                    const token_t *ptok = &st->tokens->items[k];
                    if (ptok->kind == TOKEN_EOF) {
                        break;
                    }
                    if (ptok->kind == TOKEN_NEWLINE) {
                        k++;
                        continue;
                    }
                    if (token_is_symbol(ptok, '(')) {
                        depth_inside++;
                        k++;
                        continue;
                    }
                    if (token_is_symbol(ptok, ')')) {
                        if (depth_inside == 0) {
                            past_closing = true;
                            break;
                        }
                        depth_inside--;
                        k++;
                        continue;
                    }
                    if (token_is_symbol(ptok, ',') && depth_inside == 0) {
                        k++;
                        continue;
                    }
                    /* variadic */
                    if (token_is_symbol(ptok, '.')) {
                        fn.is_variadic = true;
                        k++;
                        continue;
                    }
                    /* Try to parse a type at current position */
                    margo_type_t ptype;
                    size_t type_end = k;
                    if (sema_parse_type(st->source, st->tokens, &type_end, &ptype)) {
                        /* Next token should be the parameter name */
                        size_t nm = skip_nl(st->tokens, type_end);
                        char *pname = NULL;
                        if (nm < st->tokens->count &&
                            st->tokens->items[nm].kind == TOKEN_IDENTIFIER) {
                            /* Skip array brackets if present */
                            size_t af = skip_nl(st->tokens, nm + 1);
                            if (af < st->tokens->count && token_is_symbol(&st->tokens->items[af], '[')) {
                                /* consume until ']' */
                                while (af < st->tokens->count &&
                                       !token_is_symbol(&st->tokens->items[af], ']')) {
                                    af++;
                                }
                                af++;
                                k = af;
                            } else {
                                k = nm + 1;
                            }
                            pname = sema_dup_range(st->source,
                                                   st->tokens->items[nm].offset,
                                                   st->tokens->items[nm].offset +
                                                   st->tokens->items[nm].length);
                        } else {
                            k = type_end;
                        }
                        /* Grow arrays */
                        if (fn.param_count == param_cap) {
                            size_t nc = param_cap ? param_cap * 2 : 4;
                            margo_type_t *nt = realloc(fn.param_types, nc * sizeof(margo_type_t));
                            char       **nn = realloc(fn.param_names, nc * sizeof(char *));
                            if (!nt || !nn) {
                                /* If one realloc succeeded, update the pointer so sema_func_free
                                 * can release the correctly-reallocated block.  realloc failure
                                 * leaves the original pointer valid, so we only need to handle
                                 * the case where one succeeded and the other did not. */
                                if (nt) {
                                    fn.param_types = nt;
                                }
                                if (nn) {
                                    fn.param_names = nn;
                                }
                                sema_func_free(&fn);
                                sema_type_free(&ptype);
                                free(pname);
                                return false;
                            }
                            fn.param_types = nt;
                            fn.param_names = nn;
                            param_cap      = nc;
                        }
                        fn.param_types[fn.param_count] = ptype;
                        fn.param_names[fn.param_count] = pname;
                        fn.param_count++;
                    } else {
                        k++;
                    }
                }
                (void)past_closing;
                return funcreg_push(&st->funcreg, fn);
            }
            /* Not followed by '(' – might be the first token of return type */
            margo_type_t parsed;
            if (sema_parse_type(st->source, st->tokens, &i, &parsed)) {
                sema_type_free(&return_type);
                return_type = parsed;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    return true;
}

/* =========================================================================
 * Variable declaration detection
 * ====================================================================== */

/**
 * @brief Attempt to detect a variable declaration at position *i*.
 *
 * Returns true when a declaration was found (and possibly registered in
 * both the symbol table and the RAII table).
 */
static bool try_register_var_decl(sema_state_t *st, size_t i) {
    /* Pattern: <type> <ident> [= <expr>]
     * The presence of a type token before an identifier strongly suggests
     * a declaration.  We are conservative: if type parsing fails we do
     * nothing. */
    size_t type_end = i;
    margo_type_t vtype;
    if (!sema_parse_type(st->source, st->tokens, &type_end, &vtype)) {
        return false;
    }
    size_t j = skip_nl(st->tokens, type_end);
    if (j >= st->tokens->count) {
        sema_type_free(&vtype);
        return false;
    }
    const token_t *name_tok = &st->tokens->items[j];
    if (name_tok->kind != TOKEN_IDENTIFIER) {
        sema_type_free(&vtype);
        return false;
    }
    /* Make sure the next token after the name is =, ;, newline, [, or ) */
    size_t k = skip_nl(st->tokens, j + 1);
    bool followed_by_assign = false;
    if (k < st->tokens->count) {
        if (token_is_symbol(&st->tokens->items[k], '=')) {
            followed_by_assign = true;
        } else if (!token_is_symbol(&st->tokens->items[k], '(')) {
            /* likely function call or other expression; fall through */
        }
    }
    /* Register in symbol table */
    char name_buf[128] = {0};
    size_t name_len = name_tok->length < sizeof(name_buf) - 1 ? name_tok->length : sizeof(name_buf) - 1;
    memcpy(name_buf, name_tok->lexeme, name_len);

    /* Check if the RHS is alloc/alloc_and_init */
    bool is_alloc_owned = false;
    if (followed_by_assign) {
        size_t rhs = skip_nl(st->tokens, k + 1);
        if (rhs < st->tokens->count) {
            const token_t *rhs_tok = &st->tokens->items[rhs];
            if (rhs_tok->kind == TOKEN_IDENTIFIER &&
                ((strncmp(rhs_tok->lexeme, "alloc_and_init", rhs_tok->length) == 0 && rhs_tok->length == 14) ||
                 (strncmp(rhs_tok->lexeme, "alloc",         rhs_tok->length) == 0 && rhs_tok->length == 5))) {
                is_alloc_owned = true;
                vtype.is_owned = true;
            }
        }
    }

    symtab_push(&st->symtab, name_buf, vtype, st->brace_depth, false, name_tok->line);

    if (is_alloc_owned && st->raii) {
        raii_table_push(st->raii, name_buf, st->brace_depth, name_tok->line);
    }

    return true;
}

/**
 * @brief Detect a bare reassignment of an already-owned variable:
 *        `name = alloc(...)` where `name` is already in the symbol table
 *        and already alloc-owned.  We flag this as a potential ownership
 *        transfer so the transpiler can insert a free before the reassign.
 */
static void check_reassignment_ownership(sema_state_t *st, size_t i) {
    const token_t *tok = &st->tokens->items[i];
    if (tok->kind != TOKEN_IDENTIFIER) {
        return;
    }
    size_t j = skip_nl(st->tokens, i + 1);
    if (j >= st->tokens->count || !token_is_symbol(&st->tokens->items[j], '=')) {
        return;
    }
    /* Ensure it is not == */
    if (j + 1 < st->tokens->count && token_is_symbol(&st->tokens->items[j + 1], '=')) {
        return;
    }
    size_t k = skip_nl(st->tokens, j + 1);
    if (k >= st->tokens->count) {
        return;
    }
    const token_t *rhs_tok = &st->tokens->items[k];
    if (rhs_tok->kind != TOKEN_IDENTIFIER) {
        return;
    }
    bool is_alloc_rhs =
        (strncmp(rhs_tok->lexeme, "alloc_and_init", rhs_tok->length) == 0 && rhs_tok->length == 14) ||
        (strncmp(rhs_tok->lexeme, "alloc",         rhs_tok->length) == 0 && rhs_tok->length == 5);
    if (!is_alloc_rhs) {
        return;
    }
    /* Variable already in RAII table? */
    if (!st->raii) {
        return;
    }
    char name_buf[128] = {0};
    size_t name_len = tok->length < sizeof(name_buf) - 1 ? tok->length : sizeof(name_buf) - 1;
    memcpy(name_buf, tok->lexeme, name_len);
    for (size_t idx = 0; idx < st->raii->count; ++idx) {
        if (strcmp(st->raii->items[idx].name, name_buf) == 0 &&
            !st->raii->items[idx].transferred) {
            /* Duplicate entry: the same name is being reassigned.
             * We mark the old entry as transferred so the transpiler can
             * insert an explicit free before the assignment. */
            st->raii->items[idx].transferred = true;
            /* Re-register under the same scope depth so the closing brace
             * frees it again. */
            raii_table_push(st->raii, name_buf, st->brace_depth, tok->line);
            return;
        }
    }
    /* Not previously seen: register as new owned var. */
    raii_table_push(st->raii, name_buf, st->brace_depth, tok->line);
}

/* =========================================================================
 * Function call arity validation
 * ====================================================================== */

static void check_call_arity(sema_state_t *st, size_t call_idx) {
    const token_t *fn_tok = &st->tokens->items[call_idx];
    char name_buf[128] = {0};
    size_t name_len = fn_tok->length < sizeof(name_buf) - 1 ? fn_tok->length : sizeof(name_buf) - 1;
    memcpy(name_buf, fn_tok->lexeme, name_len);

    sema_func_t *sig = funcreg_lookup(&st->funcreg, name_buf);
    if (!sig) {
        return;
    }
    if (sig->is_variadic) {
        return;
    }

    /* Count actual arguments by scanning the argument list. */
    size_t j = skip_nl(st->tokens, call_idx + 1);
    if (j >= st->tokens->count || !token_is_symbol(&st->tokens->items[j], '(')) {
        return;
    }
    j++;
    size_t arg_count  = 0;
    bool   has_arg_content  = false;
    int    paren_depth = 1;
    while (j < st->tokens->count) {
        const token_t *t = &st->tokens->items[j];
        if (t->kind == TOKEN_EOF) {
            break;
        }
        if (t->kind == TOKEN_NEWLINE) {
            j++;
            continue;
        }
        if (token_is_symbol(t, '(')) {
            paren_depth++;
            if (!has_arg_content) {
                has_arg_content = true;
            }
            j++;
            continue;
        }
        if (token_is_symbol(t, ')')) {
            paren_depth--;
            if (paren_depth == 0) {
                if (has_arg_content) {
                    arg_count++;
                }
                break;
            }
            j++;
            continue;
        }
        if (token_is_symbol(t, ',') && paren_depth == 1) {
            arg_count++;
            j++;
            continue;
        }
        if (!has_arg_content) {
            has_arg_content = true;
        }
        j++;
    }

    if (arg_count != sig->param_count) {
        sema_diag_push(st->diags, fn_tok->line, true,
                       "function '%s' expects %zu argument(s) but %zu given",
                       name_buf, sig->param_count, arg_count);
    }
}

/* =========================================================================
 * `weird` dimension validation
 * ====================================================================== */

/**
 * @brief Warn when a `weird(T, N)` literal uses a dimension outside [0, 8].
 */
static void check_weird_dim(sema_state_t *st, size_t weird_idx) {
    /* weird ( T , N ) */
    size_t j = skip_nl(st->tokens, weird_idx + 1);
    if (j >= st->tokens->count || !token_is_symbol(&st->tokens->items[j], '(')) {
        return;
    }
    /* Skip to the comma */
    int depth = 1;
    j++;
    while (j < st->tokens->count && depth > 0) {
        if (token_is_symbol(&st->tokens->items[j], '(')) {
            depth++;
        } else if (token_is_symbol(&st->tokens->items[j], ')')) {
            depth--;
            if (depth == 0) {
                return;
            }
        } else if (token_is_symbol(&st->tokens->items[j], ',') && depth == 1) {
            j++;
            break;
        }
        j++;
    }
    j = skip_nl(st->tokens, j);
    if (j >= st->tokens->count || st->tokens->items[j].kind != TOKEN_NUMBER) {
        return;
    }
    int dim = (int)strtol(st->tokens->items[j].lexeme, NULL, 10);
    if (dim < 0 || dim > 8) {
        sema_diag_push(st->diags, st->tokens->items[weird_idx].line, true,
                       "weird pointer dimension %d is out of supported range [0, 8]", dim);
    }
}

/* =========================================================================
 * Main analysis entry point
 * ====================================================================== */

bool sema_run(const char           *source,
              const token_buffer_t *tokens,
              raii_table_t         *raii_out,
              sema_diag_list_t     *diags_out,
              diagnostic_t         *diag) {
    if (!tokens || !raii_out || !diags_out) {
        return false;
    }
    memset(raii_out,  0, sizeof(*raii_out));
    memset(diags_out, 0, sizeof(*diags_out));

    sema_state_t st;
    memset(&st, 0, sizeof(st));
    st.source  = source;
    st.tokens  = tokens;
    st.raii    = raii_out;
    st.diags   = diags_out;

    /* First pass: collect all fn signatures for forward-call validation. */
    for (size_t i = 0; i < tokens->count; ++i) {
        const token_t *tok = &tokens->items[i];
        if (tok->kind == TOKEN_EOF) {
            break;
        }
        if (tok->kind == TOKEN_IDENTIFIER &&
            strncmp(tok->lexeme, "fn", tok->length) == 0 && tok->length == 2) {
            if (!collect_fn_signature(&st, i)) {
                diagnostic_set(diag, tok->line, "sema: out of memory collecting fn signatures");
                symtab_free(&st.symtab);
                funcreg_free(&st.funcreg);
                return false;
            }
        }
    }

    /* Second pass: scope + symbol table + RAII + validation. */
    for (size_t i = 0; i < tokens->count; ++i) {
        const token_t *tok = &tokens->items[i];
        if (tok->kind == TOKEN_EOF) {
            break;
        }
        if (tok->kind == TOKEN_NEWLINE) {
            continue;
        }

        /* Track @style c blocks: inside them syntax is raw C so we suppress
         * declaration detection to avoid false positives. */
        if (tok->kind == TOKEN_AT) {
            size_t j = skip_nl(tokens, i + 1);
            if (j < tokens->count && token_is_identifier(&tokens->items[j], "style")) {
                st.inside_style_block = true;
            }
            continue;
        }

        /* Brace depth tracking */
        if (token_is_symbol(tok, '{')) {
            st.brace_depth++;
            continue;
        }
        if (token_is_symbol(tok, '}')) {
            /* Pop symbols at this depth */
            symtab_pop_depth(&st.symtab, st.brace_depth);
            if (st.brace_depth == 1) {
                st.in_function        = false;
                st.inside_style_block = false;
            }
            if (st.brace_depth > 0) {
                st.brace_depth--;
            }
            continue;
        }

        if (st.inside_style_block) {
            continue;
        }

        /* fn keyword: mark we are entering a function body */
        if (tok->kind == TOKEN_IDENTIFIER &&
            strncmp(tok->lexeme, "fn", tok->length) == 0 && tok->length == 2) {
            st.in_function = true;
            continue;
        }

        /* `weird` dimension check */
        if (tok->kind == TOKEN_IDENTIFIER &&
            strncmp(tok->lexeme, "weird", tok->length) == 0 && tok->length == 5) {
            check_weird_dim(&st, i);
            continue;
        }

        /* Variable declaration attempt */
        if (tok->kind == TOKEN_IDENTIFIER && st.in_function) {
            /* Attempt type+name detection */
            try_register_var_decl(&st, i);
            /* Check for bare reassignment ownership */
            check_reassignment_ownership(&st, i);
            /* Function call arity check */
            size_t j = skip_nl(tokens, i + 1);
            if (j < tokens->count && token_is_symbol(&tokens->items[j], '(')) {
                check_call_arity(&st, i);
            }
        }
    }

    symtab_free(&st.symtab);
    funcreg_free(&st.funcreg);
    return true;
}
