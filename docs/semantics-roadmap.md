# Semantic Strategy Notes

## Memory Management
- **RAII Tracker**: `raii_live_t` in `transpiler.c` maintains a stack of owned variables per brace depth.  Each `ident = alloc(...)` / `ident = alloc_and_init(...)` assignment detected during the code-generation pass pushes the variable onto the current scope.
- **Scope-Exit Injection**: When the transpiler emits `}`, `raii_live_emit_scope_frees` is called before writing the brace.  All owned variables at the closing depth are freed in reverse-declaration order.
- **Return-Path Injection**: When the transpiler encounters `return`, `raii_live_emit_return_frees` is called for all owned variables across all active scopes from the function body depth to the current depth.  The returned identifier (if any) is excluded to support ownership transfer.
- **Ownership Transfer**: If a variable being assigned is already in the RAII table, the sema pass marks the old entry as `transferred = true` and registers a fresh entry; the transpiler will then emit a free before the reassignment.
- **Escape Analysis**: A `return <ident>` pattern causes `extract_return_ident` to identify the identifier; that variable is skipped during return-path free injection, implementing the "ownership escapes via return" rule.

## Type System (sema.c)
- **Type Representation**: `margo_type_t` stores a `margo_type_kind_t` (covering all C primitives plus `string`, `auto`, `weird`, `struct`, `union`, `enum`), an optional `ptr_depth` for `weird` types, and an optional heap-allocated `name` for compound types.
- **Type Parsing**: `sema_parse_type` handles multi-word types (`unsigned long long`, `long double`), `const` qualifier, `weird(T, N)`, and struct/union/enum tags.
- **`weird` Dimension Validation**: Any `weird(T, N)` with `N` outside `[0, 8]` produces a hard semantic error.
- **Future**: struct field access validation requires parsing C headers via libclang; tracked as a future milestone.

## Control Flow
- **Implicit Loop Variables**: Parser creates a hidden `VarDecl` node scoped to the `for` loop body.  The code generator treats `for x=0; <10; ++` as sugar for `for (int x=0; x<10; ++x)`.
- **Conditional Increment Slot**: Increment expression is parsed as a general expression AST node so constructs like `count.odd? ++ : +=2` can be compiled into conditional IR blocks (reserved for a future lowering pass).

## Directives & FFI
- **Directive Table**: During parsing, each `@directive` registers a handler that may mutate parser state (`@style c`), import bindings (`@import c/...`), or set capability flags (`@import godmode`).
- **`godmode`**: Expands to ~20 standard C headers.  Intended for programs like the self-hosting compiler that need comprehensive standard library access without verbose per-header imports.
- **`c++/` Stub**: `@import c++/path` is preserved as a C comment.  Full C++ class/function binding via libclang is a future milestone.
- **Extended `std/` Modules**: `std/string`, `std/math`, `std/stdlib`, `std/time`, `std/assert`, `std/errno` are now mapped directly to their C header counterparts.
- **`null` Keyword**: Lowered to C `NULL` during the code-gen pass; no include required.
- **C/C++ Imports**: Use libclang to parse headers/classes and store them as foreign symbols referencing the exact ABI name (future).
- **Autocorrect Toggle**: Directive `@set autocorrect on/off` flips a flag consumed by the identifier-resolution pass; the pass consults the fzf-backed suggester when enabled (future milestone).

## Multi-Error Diagnostics
- `sema_diag_list_t` accumulates `sema_diag_entry_t` records (line, message, severity) across the entire semantic pass before any errors are reported.
- Errors trigger aborting compilation after the sema pass prints all messages; warnings do not abort.
- Output format: `<path>:<line>: error: <message>` for IDE compatibility.
