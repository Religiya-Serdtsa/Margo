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

`make` now builds `build/margo-c` (C bootstrap) and then promotes the self-compiled binary to `build/margo` by default.

Use `--clang <path>` or `CLANG=<path>` to pick a specific LLVM toolchain, and append `--emit-llvm number_baseball.ll` if you need to inspect the generated IR.

### Self-compilation experiment

`experiment/margo_compiler.margo` now embeds the full compiler core via `@style c`,
so the binary produced from this file can rebuild itself without delegating to the
bootstrap executable. The default `make` target already runs this flow; you can also
run it explicitly with:

```bash
make margo-in-margo
```

This produces:
- `margo_build/margo_stage1` — built by `build/margo-c`
- `margo_build/margo` — rebuilt by `margo_stage1` (and then rebuilt once more for stability)
- `build/margo` — promoted self-compiled compiler
- `build/margo-c` — C bootstrap compiler

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
- `mat_find(arr, target, precision) { ... }` and `mat_for(arr, fn_name)` provide matrix-aware traversal sugar: the compiler infers the rank/shape, executes the block only when an approximate match is discovered, and feeds all index coordinates to the handler without requiring hand-written nested loops, all while flattening tensors in a linear-algebra-friendly order for cache locality.
- `mat_neighbor()` extends the same idea—inside `mat_for` it captures the ambient indices to return a radius-1 neighbor view, while top-level calls such as `mat_neighbor(arr, i, j, ..., n | seek_fn = fn)` request arbitrary radii or predicate-driven neighborhoods with automatic boundary clamping.
- Byte-pattern literals like `[0x7F, 'E', 'L', 'F']` are lowered to TU-local `static const uint8_t` buffers and can be passed straight into the file helpers without manual array declarations.
- Python-style enumerations `for value in [1, 2, 3, 4] { ... }` lower to synthesized constant arrays and ordinary loops so inline lists stay ergonomic.

The number baseball example now uses the new loop sugar so that its structure matches the concept draft more closely.

### Showcase Snippets

```margo
@import file/core
@import std/io

fn dump_zip_offsets(string path) {
    weird(FILE, 1) fp = fopen(path, "rb")
    if !fp return

    bool hits[] = return_all_bitmask_offsets(fp, [0x50, 0x4B, 0x03, 0x04])
    for slot in [0, 32, 64, 96] {
        print("slot ", slot, ": ", hits[slot])
    }
    for pos in hits.indices() {
        if hits[pos] print("central dir? @", pos)
    }
    fclose(fp)
}
```

```margo
fn sum_hotspots(auto_matrix field) {
    mat_find(field, 10, 0.001) {
        print("exact 10 at", mat_index[0], mat_index[1])
    }
    mat_for(field, fn(i, j, cell) bool {
        auto neigh = mat_neighbor()
        auto spikes = mat_neighbor(field, i, j, 2, seek_fn = fn(nc) bool { return nc.value > cell })
        neigh.for_each(fn(nc) {
            if nc.value > cell * 2 print("spike", nc.index[0], nc.index[1])
        })
        if spikes.count == 0 && cell > 0 {
            print("isolated hot cell", i, j)
        }
        return true
    })
}
```

## Self-Hosting Minimum Support

The following features have been implemented to allow Margo programs to write a Margo compiler (self-hosting subset).

### RAII Scope-Exit Free Injection
Variables assigned from `alloc` / `alloc_and_init` are automatically freed when their enclosing scope closes:

```margo
fn process() {
    string buf = alloc_and_init(256, "hello")
    // ... work ...
}  // ← free(buf) is injected here automatically
```

Return-path injection: every `return` statement also emits `free()` for all live owned variables.  The variable being returned is skipped (ownership transfer).

### `null` Keyword
Lowered to the C `NULL` macro — no `#include` required:
```margo
if ptr == null { ... }
```

### `@import godmode`
Expands to ~20 standard C headers, covering the full POSIX + C standard library surface:
```margo
@import godmode
```

### `@import c++/...` Stub
C++ binding support is a future milestone.  The directive is preserved as a comment so existing source stays parseable.

### Extended `@import std/...` Modules
| Directive | Maps to |
| --- | --- |
| `@import std/io` | `<stdio.h>` |
| `@import std/mem` | `<string.h>` |
| `@import std/string` | `<string.h>` |
| `@import std/math` | `<math.h>` |
| `@import std/stdlib` | `<stdlib.h>` |
| `@import std/time` | `<time.h>` |
| `@import std/assert` | `<assert.h>` |
| `@import std/errno` | `<errno.h>` |
| `@import std/file` | `<stdio.h>` |
| `@import file/core` | `<stdio.h>` + binary pattern helpers |

### File Pattern Helpers (`@import file/core`)
- `seek_from_file(stream, pattern)` scans from the current `FILE *` position to EOF, returning `true` when the byte pattern is present while restoring the original file pointer afterward.
- `pos_from_file(stream, pattern)` exposes the first match offset as a `long` (`-1` if absent) without moving the pointer.
- `jmp_from_file(stream, pattern)` combines the two: it locates the pattern and performs `fseek(stream, offset, SEEK_SET)` on success, returning a boolean for convenience.
- `return_all_bitmask_offsets(stream, pattern)` scans the whole file once, producing a RAII `bitmask_view { bool *bits; size_t len; }` whose entries mark every matching offset. You can capture it via `bool hits[] = return_all_bitmask_offsets(...)` and iterate over `hits.indices()`.
- Patterns accept either buffers plus lengths or inline literals such as `[0x7F, 'E', 'L', 'F']`; the compiler hoists those literals into deduplicated `static const uint8_t` arrays automatically.

### Thread/Process Runtime (Experimental)
- `@import threads/core` — exposes a header-only worker scheduler built on top of `pthread`. Provides `threads_cluster_t`, `threads_mailbox_t`, and helpers such as `threads_spawn`, `threads_spawn_isolate`, and `threads_mailbox_send/recv`.
- `@import process` / `@import process/core` — wraps POSIX `fork`, shared memory, and futex-friendly channels via `process_channel_t`/`process_cluster_t`. Mimics Go-style `chan <- val` semantics in plain C.
- The new `examples/thread_process_bench` folder wires both modules together to provide a combined throughput benchmark.

### Semantic Analysis Pass (`src/sema.c`)
Runs over the token stream before code generation:
- **Function signature registry**: records all `fn` declarations; validates call-site argument counts.
- **`weird` dimension validation**: rejects pointer dimensions outside `[0, 8]`.
- **RAII ownership table**: detects `ident = alloc(...)` patterns and feeds the transpiler.
- **Multi-error accumulation**: collects all diagnostics before halting, enabling IDE-style error output.

## Contribution Notes (Draft)

- Document new ideas under `docs/` before wiring them into the compiler/runtime.
- Keep comments in English as mandated by the spec.
- Plan for an LLVM-based backend when designing the type, memory, and interop layers.

Further details live in `TODO.md`.
