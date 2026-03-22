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

## 6. Semantic Analysis Pass
- Runs over the token stream before code generation, performing:
  - **Function signature registry**: all `fn` declarations are recorded; call sites are checked for arity mismatches.
  - **Symbol table with scope tracking**: variable declarations are registered per brace-depth scope.
  - **RAII ownership table**: every `ident = alloc(...)` pattern is recorded; used by the transpiler for free injection.
  - **`weird` dimension validation**: reports an error if dimension is outside [0, 8].
  - **Multi-error accumulation**: all diagnostics are collected into a `sema_diag_list_t` and printed together before compilation halts on errors.

## 7. Integrated Tools
- `@set autocorrect on` delegates unknown identifier resolution to an fzf-backed suggestion engine during compilation.

This outline feeds directly into the actionable roadmap captured in `TODO.md`.
