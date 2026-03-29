# Spec Outline (Implementation View)

## 1. Philosophy
- Preserve raw C layouts to avoid marshalling; compiler must default to C ABI for every exposed function.
- Enforce RAII semantics at compile time to satisfy "Zero-Grief" memory handling.
- Keep frontend fast (interpreter-like dev loop) while generating LLVM IR for production builds.

## 2. Type System
- Scalar set mirrors C: `int`, `char`, `float`, `double`, plus `void` for explicit lack of return values.
- `string` aliases `char *` but the allocator/runtime appends and tracks `\0` automatically.
- `auto` triggers inference based on the initializer expression; inference happens after semantic analysis so pointer decay is visible.
- `weird(T, n)` lowers to `T` with `n` pointer stars; the semantic pass validates that `n` is in [0, 8] and records dimension metadata for call-site diagnostics.
- Integer families: `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` and the `int*_t` variants are first-class Margo types.
- `size_t` is recognized as a distinct type to avoid implicit narrowing warnings when interfacing with the C standard library.
- Byte-pattern literals `[0x01, 0xFF, ...]` lower to TU-local `static const uint8_t` buffers plus `(uint8_t *, size_t)` views. Identical literals are deduplicated so file helpers can share backing storage.
- List literals `[expr1, expr2, ...]` used inside `for … in` loops evaluate left-to-right at compile time, materialize into TU-local constant buffers, and expose a synthesized slice so the loop variable can range over the values.

## 3. Memory Management
- Built-in `alloc` / `alloc_and_init` produce ownership-bound handles; the transpiler's RAII tracker injects `free` at scope-exit by tracking assignment patterns of the form `ident = alloc(...)`.
- Scope-exit injection: when a closing `}` is emitted, all owned variables at that brace depth are freed in reverse-declaration order before the brace.
- Return-path injection: any `return` statement causes the transpiler to emit `free` for every owned variable visible at that point; if the return expression is a bare identifier, that identifier is skipped (ownership transfer).
- RAII tracking is conservative: only direct top-level assignments are detected; pointer arithmetic or aliasing is left to the C compiler's sanitizers.
- `null` is a first-class Margo keyword that lowers to the C `NULL` macro, removing the need for an explicit `@import c/stddef`.

## 4. Control Flow
- `for x=0; <N; ++ { ... }`: the unnamed loop variable (`x` here) is implicit and scoped to the loop body.
- `if v == 1 return 100`: parenthesis-free headers get wrapped during parsing so `return`/`break`/`continue`-style statements keep concept syntax while the emitted C receives `if (v == 1)`.
- `else` / `else if` chains after paren-free `if` pass through unchanged since the rewrites preserve compatible C output.
- Increment slot accepts expression-like statements, enabling `count.odd? ++ : +=2` semantics (reserved for future lowering pass).
- `mat_find(matrix_expr, target_expr, precision_expr = 0) { ... }`:
  - Frontend derives the full rank/extent for `matrix_expr` from its static type metadata (including `weird` sugar). No manual limits or nested loops are required in source.
  - Emits helper loops that linearly scan the matrix, compare each value via `abs(value - target) <= precision`, and execute the attached block only when a match exists.
  - The block receives two implicit read-only temporaries: `mat_value` (the matching element) and `mat_index` (a small struct exposing `rank` plus array-like `mat_index[i]` access for each coordinate).
- `mat_for(matrix_expr, handler_ident)`:
  - Parser expands the construct into rank-aware nested loops and generates a call to `handler_ident` per element.
  - The handler signature declares as many leading integer parameters as the matrix rank; an optional trailing parameter typed as the element value can also be requested.
  - If the handler returns `bool`, `false` terminates the traversal early; any other return type simply discards the value and keeps iterating.
  - The generated loops treat the tensor as a flattened linear-algebra tile so future backends can vectorize/cache-optimize automatically.
- `mat_neighbor(...)` helper:
  - Inside a `mat_for` body, `mat_neighbor()` (with no args) captures the ambient matrix handle and current coordinate tuple, producing a neighbor view with default radius `n = 1`.
  - Standalone calls follow `mat_neighbor(arr, idx1, idx2, ..., radius? , seek_fn = optional_fn)`; the compiler treats trailing named `seek_fn = …` as a filter callback and assumes every unnamed argument is an index. Without `seek_fn`, the final positional argument becomes the radius `n`.
  - Radius is interpreted as an L∞ band: every axis range `[coord - n, coord + n]` is enumerated, and out-of-bounds coordinates are clamped away automatically.
  - When a `seek_fn` is supplied, it is invoked with a `mat_neighbor_cell` struct (`.index`, `.value`, `.distance`). Only cells returning `true` are retained.
  - The resulting view exposes `.count`, random access (`view[i].value`, `view[i].index[d]`), and a `for_each` helper that short-circuits if the callback yields `false`.
- Matrix core helpers (`@import matrix/core`):
- `matrix_fill(rows, cols, value)` allocates a fresh RAII-managed `auto_matrix` and initializes every entry to `value`. The handle behaves like a plain array (`grid[i][j]`) and plugs into `mat_*` constructs.
- `matrix_identity(size, diag_value = 1)` emits an identity matrix of arbitrary order with the diagonal preset to `diag_value` and the rest zeroed.
- `matrix_mul(lhs, rhs)` multiplies two dense matrices (`lhs` m×k, `rhs` k×n) and returns a new buffer; mismatched dimensions trigger diagnostics during semantic analysis.
- `matrix_transpose(matrix)` returns a zero-copy view with swapped axes so the original storage can be reused.
- `matrix_map(matrix, fn mapper)` synthesizes a same-shaped matrix by running `mapper(mat_neighbor_cell cell)` per element, providing easy access to coordinates plus the original value.
- Procedural helpers (`matrix_rows`, `matrix_cols`, `matrix_get`/`matrix_set`, `matrix_neighbor` + `neighbor_view_for_each`) expose shape-aware iterators so imperative code can traverse matrices or inspect adjacent cells without leaving Margo.
- `for value in [ ... ] { ... }` loops:
  - Parser lowers literal enumerations into synthesized constant buffers and emits classic `for` loops over their indices.
  - Loop variables default to `__auto_type`, are read-only, and support heterogenous inputs by promoting to a shared super type.

## 5. Directives & FFI
- `@import c/name` → `#include <name.h>` (or `#include <name>` when `.h` suffix is explicit).
- `@import c++/path` → stub comment; C++ binding support is a future milestone.
- `@import godmode` → expands to a comprehensive set of standard C headers (`<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<stdint.h>`, `<unistd.h>`, and more) for programs that require the full standard library.
- `@import std/io` → `<stdio.h>` (also enables `Scan` / `ScanLine` builtins).
- `@import std/mem` → `<string.h>`.
- `@import std/string` → `<string.h>`.
- `@import std/math` → `<math.h>`.
- `@import std/stdlib` → `<stdlib.h>`.
- `@import std/time` → `<time.h>`.
- `@import std/assert` → `<assert.h>`.
- `@import std/errno` → `<errno.h>`.
- `@style c { ... }` toggles strict C-like parsing for easy code importing.
- Decorators such as `@use_switch_optim` hint optimizer passes (emitted as C comments).
- `@set autocorrect on` delegates unknown identifier resolution to an fzf-backed suggestion engine.
- `@import threads/core` injects the header-only threading runtime that powers `threads <name> { locked_var { ... } threadN ... }`, exposing only `threadN.init/join`, `threads.<name>.join_all(chan)`, `threads.<name>.sync()`, and the four decorators (`@chantype`, `@nochan`, `@independent`, `@lazyjoin`).
- `@import process` / `@import process/core` inject the POSIX process/channel helpers that emulate Go-style `chan`.

## 6. Semantic Analysis Pass
- Runs over the token stream before code generation, performing:
  - **Function signature registry**: all `fn` declarations are recorded; call sites are checked for arity mismatches.
  - **Symbol table with scope tracking**: variable declarations are registered per brace-depth scope.
  - **RAII ownership table**: every `ident = alloc(...)` pattern is recorded; used by the transpiler for free injection.
  - **`weird` dimension validation**: reports an error if dimension is outside [0, 8].
  - **Multi-error accumulation**: all diagnostics are collected into a `sema_diag_list_t` and printed together before compilation halts on errors.

## 7. Integrated Tools
- `@set autocorrect on` delegates unknown identifier resolution to an fzf-backed suggestion engine during compilation.

## 8. File Pattern Helpers (`@import file/core`)
- Module exposes `file_pattern_t { uint8_t *data; size_t len; }` plus helper functions built on top of `<stdio.h>`.
- `[0xDE, 0xAD]` literals automatically materialize as `file_pattern_t` temporaries, but callers can also pass explicit buffers and lengths.
- `seek_from_file(FILE *f, file_pattern_t pat)` streams from the current file position to EOF, returns `true` on the first match, and restores the original file pointer regardless of the outcome.
- `pos_from_file(FILE *f, file_pattern_t pat)` behaves similarly but returns the first match offset as `long` (`-1` if not found) without moving the pointer.
- `jmp_from_file(FILE *f, file_pattern_t pat)` reuses the position search and, on success, executes `fseek(f, offset, SEEK_SET)`; failure leaves the pointer untouched.
- All helpers guard against `NULL` streams or empty patterns, ensure offsets remain within the file’s mathematical bounds, and rely on `ftell`/`fseek` so sandboxed IO stays portable.
- `bitmask_view return_all_bitmask_offsets(...)` scans the entire file once, builds a boolean mask for every byte offset, and returns `{ bool *bits; size_t len; }`. The view exposes `.indices()` so `for index in hits.indices()` lowers cleanly, and it is freed automatically via RAII.

This outline feeds directly into the actionable roadmap captured in `TODO.md`.
