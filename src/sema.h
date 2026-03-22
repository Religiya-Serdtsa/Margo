#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "diagnostics.h"
#include "lexer.h"

/**
 * @file sema.h
 * @brief Semantic analysis pass: type system representation, symbol table,
 *        function-signature registry, RAII ownership tracking, and a
 *        multi-error diagnostic list for compiler-quality error reporting.
 *
 * The pass runs over the token stream produced by the lexer and performs
 * checks that C's own type checker cannot easily surface at the Margo
 * abstraction level (e.g. `weird` pointer dimension mismatches, alloc
 * ownership transfer, arity mismatches against Margo-defined functions).
 * It intentionally stays read-only with respect to the token stream; all
 * transformations belong to the transpiler.
 */

/* -------------------------------------------------------------------------
 * Type system
 * ---------------------------------------------------------------------- */

/**
 * @brief Every distinct type category understood by the semantic pass.
 *
 * MARGO_TYPE_UNKNOWN is the initial state; it propagates through `auto`
 * until the initializer is resolved.  MARGO_TYPE_WEIRD wraps a base type
 * with a pointer depth tracked in `margo_type_t.ptr_depth`.
 */
typedef enum {
    MARGO_TYPE_UNKNOWN = 0,
    MARGO_TYPE_VOID,
    MARGO_TYPE_INT,
    MARGO_TYPE_LONG,
    MARGO_TYPE_LONGLONG,
    MARGO_TYPE_UINT,
    MARGO_TYPE_ULONG,
    MARGO_TYPE_ULONGLONG,
    MARGO_TYPE_CHAR,
    MARGO_TYPE_UCHAR,
    MARGO_TYPE_SHORT,
    MARGO_TYPE_USHORT,
    MARGO_TYPE_FLOAT,
    MARGO_TYPE_DOUBLE,
    MARGO_TYPE_LDOUBLE,
    MARGO_TYPE_BOOL,
    MARGO_TYPE_STRING,  /**< Margo `string` alias (char *) */
    MARGO_TYPE_AUTO,    /**< Unresolved; filled in by inference */
    MARGO_TYPE_WEIRD,   /**< T with ptr_depth pointer stars */
    MARGO_TYPE_STRUCT,
    MARGO_TYPE_UNION,
    MARGO_TYPE_ENUM,
    MARGO_TYPE_FUNC,
    MARGO_TYPE_SIZE_T,
    MARGO_TYPE_UINT8,
    MARGO_TYPE_UINT16,
    MARGO_TYPE_UINT32,
    MARGO_TYPE_UINT64,
    MARGO_TYPE_INT8,
    MARGO_TYPE_INT16,
    MARGO_TYPE_INT32,
    MARGO_TYPE_INT64,
} margo_type_kind_t;

/**
 * @brief Compact type descriptor.
 *
 * For MARGO_TYPE_WEIRD the base kind is stored in `base_kind` and the
 * number of pointer stars in `ptr_depth`.  For struct/union/enum the
 * declared name is stored in `name`.  All other fields default to zero/NULL.
 */
typedef struct {
    margo_type_kind_t kind;
    margo_type_kind_t base_kind; /**< Only valid when kind == MARGO_TYPE_WEIRD */
    int               ptr_depth; /**< Number of pointer indirections */
    char             *name;      /**< Struct/union/enum tag; heap-allocated */
    bool              is_const;
    bool              is_owned;  /**< True when variable was alloc-initialised */
} margo_type_t;

/**
 * @brief Parse a Margo type annotation from the token stream beginning at
 *        *index*.  On success *index* is advanced past the type tokens and
 *        *out* is filled in.
 */
bool sema_parse_type(const char *source,
                     const token_buffer_t *tokens,
                     size_t *index,
                     margo_type_t *out);

/**
 * @brief Release any heap memory owned by a type descriptor.
 */
void sema_type_free(margo_type_t *t);

/**
 * @brief Return the human-readable name of a type kind (for diagnostics).
 */
const char *sema_type_kind_name(margo_type_kind_t kind);

/* -------------------------------------------------------------------------
 * Symbol table
 * ---------------------------------------------------------------------- */

/**
 * @brief One entry in a scope's symbol table.
 */
typedef struct {
    char         *name;       /**< Identifier text; heap-allocated */
    margo_type_t  type;
    int           scope_depth;
    bool          is_param;
    size_t        decl_line;
} sema_symbol_t;

/* -------------------------------------------------------------------------
 * Function signature registry
 * ---------------------------------------------------------------------- */

/**
 * @brief Recorded signature of a Margo `fn` declaration.
 */
typedef struct {
    char          *name;         /**< Function name; heap-allocated */
    margo_type_t   return_type;
    margo_type_t  *param_types;  /**< Heap-allocated array */
    char         **param_names;  /**< Heap-allocated parallel array */
    size_t         param_count;
    size_t         decl_line;
    bool           is_variadic;
} sema_func_t;

/* -------------------------------------------------------------------------
 * RAII ownership table
 * ---------------------------------------------------------------------- */

/**
 * @brief Single alloc-owned variable tracked across the transpile pass.
 *
 * The transpiler inserts a `free()` call before the closing brace of the
 * scope identified by `scope_depth` and also before any `return` statement
 * within that scope.
 */
typedef struct {
    char   name[128]; /**< Variable identifier */
    int    scope_depth;
    size_t decl_line;
    bool   transferred; /**< Set when ownership passes via return */
} raii_entry_t;

/**
 * @brief Dynamic table of all alloc-owned variables found during analysis.
 */
typedef struct {
    raii_entry_t *items;
    size_t        count;
    size_t        capacity;
} raii_table_t;

/* -------------------------------------------------------------------------
 * Multi-error diagnostic list
 * ---------------------------------------------------------------------- */

/**
 * @brief Non-fatal semantic warning or error suitable for multi-error output.
 */
typedef struct {
    size_t line;
    char   message[256];
    bool   is_error; /**< true=error, false=warning */
} sema_diag_entry_t;

/**
 * @brief Accumulates all semantic diagnostics produced by one analysis run.
 */
typedef struct {
    sema_diag_entry_t *items;
    size_t             count;
    size_t             capacity;
    size_t             error_count;
    size_t             warning_count;
} sema_diag_list_t;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Perform a full semantic analysis pass over the token stream.
 *
 * On success `raii_out` contains the ownership table (caller must call
 * `raii_table_free`) and `diags_out` contains any non-fatal diagnostics
 * (caller must call `sema_diag_list_free`).  If a fatal internal error
 * occurs (e.g. OOM) `diag` receives the message and the function returns
 * false.
 *
 * A non-zero `diags_out->error_count` means semantic errors were found but
 * the tables are still populated to allow downstream multi-error display.
 */
bool sema_run(const char       *source,
              const token_buffer_t *tokens,
              raii_table_t         *raii_out,
              sema_diag_list_t     *diags_out,
              diagnostic_t         *diag);

/**
 * @brief Release all heap memory owned by the RAII table.
 */
void raii_table_free(raii_table_t *table);

/**
 * @brief Release all heap memory owned by the diagnostic list.
 */
void sema_diag_list_free(sema_diag_list_t *list);

/**
 * @brief Print all collected diagnostics to stderr, prefixed with the
 *        source file path for IDE-compatible output.
 */
void sema_diag_list_print(const sema_diag_list_t *list, const char *source_path);
