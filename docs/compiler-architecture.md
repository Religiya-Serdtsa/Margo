# Compiler Architecture Memo

## Overview
Margo v0.1 ships as a single toolchain binary with two main entry points:

1. **`margo run`** — Fast interpreter-style execution for quick prototyping. The frontend parses and type-checks source, emits LLVM IR into an in-memory `orc` JIT, and executes `start`.
2. **`margo build`** — Ahead-of-time compiler that lowers to optimized LLVM IR, invokes `llc`/`lld`, and produces native binaries or shared objects.

Both modes share the same frontend, semantic passes, and lowering pipeline.

## Pipeline Stages

1. **Lexing/Tokenization**
   - Unicode source stored as UTF-8 but identifiers limited to ASCII.
   - Directives beginning with `@` are tokenized as distinct kinds so the parser can hand them to the directive handler.

2. **Parsing**
   - Pratt-style expression parser plus recursive-descent for declarations.
   - Loop forms support implicit variable declarations (handled during AST construction by inserting synthesized `VarDecl`).
   - `weird(T, n)` and `auto` appear as syntactic sugar nodes so later passes can desugar them.
   - Import kinds resolved at parse time: `c/`, `c++/`, `godmode`, `std/io`, `std/mem`, `std/string`, `std/math`, `std/stdlib`, `std/time`, `std/assert`, `std/errno`.

3. **AST Normalization**
   - Expand `weird(T, n)` into nested pointer types while retaining metadata (`pointed_dim`) for diagnostics.
   - Rewrite implicit loop variables into explicit scoped variables bound to the loop body.
   - Lower `null` keyword to C `NULL`.

4. **Semantic Analysis** (`src/sema.c`)
   - **Function Signature Registry**: all `fn` declarations are collected in a first pass so that call sites can be validated for arity without caring about declaration order.
   - **Symbol Table with Scope Tracking**: variable declarations are registered per brace-depth scope; symbols are popped when a scope closes.
   - **RAII Ownership Table**: every `ident = alloc(...)` / `ident = alloc_and_init(...)` assignment pattern is recorded with its scope depth; the table is consumed by the transpiler's injection pass.
   - **`weird` Dimension Validation**: pointer dimension values outside `[0, 8]` produce a hard semantic error.
   - **Call Arity Checking**: function calls to Margo-defined functions are checked against the registered parameter count; mismatches produce errors.
   - **Multi-Error Accumulation**: all diagnostics accumulate in a `sema_diag_list_t` and are printed to stderr together, enabling IDE-style multi-error output before compilation halts.

5. **Code Generation / Transpilation** (`src/transpiler.c`)
   - Translates Margo source to C, token by token, using a single-pass streaming approach.
   - **RAII injection**: a `raii_live_t` stack is maintained during the pass; `{` pushes a new scope frame; `}` pops the frame and emits `free(name)` for each owned variable before the brace; `return` emits frees for all live owned variables (skipping the returned identifier).
   - **`null` lowering**: `null` identifier tokens are replaced with `NULL` before being written to the output buffer.
   - **`@import godmode` expansion**: expanded to ~20 standard C headers covering the full POSIX + C standard library surface.
   - **`@import c++/...` stub**: preserved as a comment for future C++ binding support.
   - **Extended `@import std/...` modules**: `string`, `math`, `stdlib`, `time`, `assert`, `errno`.
   - **Implicit semicolons**: a post-processing pass inserts semicolons at line endings that do not already terminate statements.

6. **LLVM IR / Native Backend** (`src/builder.c`)
   - Invokes clang with the generated C fed via a pipe so no temp file is needed.
   - Honors the `CLANG` environment variable and the `--clang` CLI flag.
   - Links against the embedded `libttak` bundle (arena memory manager).

## Subsystems

### Directive Manager
- Maintains capability flags (e.g., `godmode`, strict `@style c` blocks).
- Exposes hooks to parser/semantic passes for localized behavior changes.

### Autocorrect Engine
- Backed by an embedded `fzf` library; receives the current scope symbol list and the unknown identifier, returns ranked suggestions.
- During `run`, unknown symbol triggers interactive prompt; on `build`, can auto-apply highest-confidence fix when allowed.

### Import Resolver
- `@import c/...` uses libclang to parse system headers and produce binding stubs.
- `@import c++/...` relies on clang's AST to expose classes/functions with name mangling preserved (future milestone).
- `@import godmode` is a shortcut that inlines the most common headers to support writing programs like the self-hosting compiler without verbose import lists.

### Diagnostics & Tooling
- All warnings/errors reference source spans; RAII injections surface as notes for debuggability.
- The semantic analysis pass (`sema.c`) collects multiple errors before stopping, giving developers a comprehensive view of all problems in a single compilation attempt.
- Language server hooks share the same AST/semantic representation to provide completions.

## Self-Hosting Roadmap

The current implementation (~4,900 lines) provides the following foundation for writing the Margo compiler in Margo:

| Feature | Status |
|---------|--------|
| RAII scope-exit free injection | ✅ Implemented |
| Return-path free injection | ✅ Implemented |
| `null` keyword | ✅ Implemented |
| `@import godmode` | ✅ Implemented |
| `@import c++/...` stub | ✅ Implemented |
| Extended `@import std/...` | ✅ Implemented |
| Semantic type tracking | ✅ Implemented (sema.c) |
| Function arity checking | ✅ Implemented |
| `weird` dimension validation | ✅ Implemented |
| Multi-error accumulation | ✅ Implemented |
| Union type support | ✅ Pass-through via `@style c` or inline C |
| Struct field access validation | 🔲 Future: requires C header parsing |
| `@set autocorrect` engine | 🔲 Future milestone |
| `@import c++/...` full binding | 🔲 Future milestone |

This memo informs the implementation TODOs and will expand as subsystems solidify.
