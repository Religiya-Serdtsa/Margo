# Margo Language

Margo is an experimental language that delivers C-native ABI compatibility while keeping prototype-level developer speed. The `concept.margo` document captures the v0.1 sketch and highlights the pillars below.

- **C-Native / Zero-Grief**: Memory layouts mirror C, and scope-based RAII removes explicit `free` calls.
- **Type System**: Standard C scalars, a convenient `string`, implicit `auto`, and the `weird(T, n)` shorthand for multi-level pointers.
- **Control Flow**: Implicit loop variables and conditional increment slots blur interpreter-like ergonomics with compiler performance.
- **Directives & Tools**: `@import`, `@style`, and `@set autocorrect` drive interop and code-style enforcement.

## Repository Layout

| Path | Description |
| --- | --- |
| `concept.margo` | Original specification draft. |
| `README.md`, `README.en.md` | Bilingual overview. |
| `docs/spec-outline.md` | Implementation-focused summary of the spec. |
| `TODO.md` | Roadmap grouped by foundational subsystems. |
| `deps/` | External playground dependencies (libttak, etc.). |
| `examples/number_baseball/` | First runnable sample written purely in Margo syntax. |

## Status

The first milestone focuses on documentation and an actionable roadmap. Completed items are marked with `[v]` inside `TODO.md`.

## Quick Start (LLVM-backed Build)

`build/margo` performs a lightweight directive pass over `.margo` sources, then feeds the code straight into LLVM's `clang` frontend (no intermediate `.c` artifacts). Convenience constructs such as `fn`, `auto`, and `weird` expand via a builtin macro prelude, after which we can emit `.ll` IR or a native binary. Supported sugar includes:

- `@import c/<header>` → `#include <header>`
- `fn [return_type] name(args) { ... }` → ordinary C functions (default return type: `int`)
- `string` alias (`typedef char *string;`), `auto` → `__auto_type`
- `weird(T, n)` → `T` followed by `n` pointer stars

```bash
make
./build/margo build examples/number_baseball/number_baseball.margo -o number_baseball
./number_baseball
```

Use `--clang <path>` or `CLANG=<path>` to pick a specific LLVM toolchain, and append `--emit-llvm number_baseball.ll` if you need to inspect the generated IR.

### Runnable Example (Number Baseball)

The `examples/number_baseball` directory contains a pure Margo-style implementation of the classic bulls-and-cows game plus a Makefile that delegates to `margo build`.

```bash
cd examples/number_baseball
make
./number_baseball
```

This showcases the new workflow without relying on `@style c` escape hatches.

### Parser-backed Syntax Experiments

The frontend now tokenizes/parses `.margo` sources before handing them to Clang, which means we can start shaping syntax that goes beyond plain C. The first batch of features includes:

- `@import c/stdio` without manually writing the `.h` suffix (the transpiler adds it for C system headers).
- Parenthesis-free loops such as `for idx=0; <N; ++ { ... }`, which get rewritten into canonical `for (idx = 0; idx < N; ++idx)` form by reusing the implicitly declared loop variable.
- Bare `if` headers like `if v == 1 return 100` automatically gain parentheses around the condition while keeping the original spacing that separates the condition from the following statement or block.
- Function-level decorators like `fn @use_switch_optim foo(...)` are preserved as comments so they no longer break the generated C.

The number baseball example now uses the new loop sugar so that its structure matches the concept draft more closely.

## Contribution Notes (Draft)

- Document new ideas under `docs/` before wiring them into the compiler/runtime.
- Keep comments in English as mandated by the spec.
- Plan for an LLVM-based backend when designing the type, memory, and interop layers.

Further details live in `TODO.md`.
