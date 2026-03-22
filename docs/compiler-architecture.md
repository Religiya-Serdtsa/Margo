# Compiler Architecture Memo (Draft)

## Overview
Margo v0.1 will ship as a single toolchain binary with two main entry points:

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

3. **AST Normalization**
   - Expand `weird(T, n)` into nested pointer types while retaining metadata (`pointed_dim`) for diagnostics.
   - Rewrite implicit loop variables into explicit scoped variables bound to the loop body.

4. **Semantic Analysis**
   - **Name Resolution:** Module-level first, then block scopes. Integrates the autocorrect suggestion provider when an identifier is missing.
   - **Type Checking:** Handles `auto` inference by solving initializer types before finalizing the declaration.
   - **RAII Ownership Tracking:** Registers each `alloc*` call result and emits deferred `free` actions tied to scope exit.

5. **LLVM IR Generation**
   - Emits C ABI-compatible layouts by mapping Margo types directly to LLVM primitives/pointers.
   - Directives like `@use_switch_optim` attach metadata to influence IR optimizations (e.g., `llvm.experimental.deoptimize` hints or jump-table lowering).

6. **Backend**
   - Shared code to emit bitcode or object files.
   - `run` path pipes IR to the LLVM ORC JIT; `build` path goes through standard codegen + lld.

## Subsystems

### Directive Manager
- Maintains capability flags (e.g., `godmode`, strict `@style c` blocks).
- Exposes hooks to parser/semantic passes for localized behavior changes.

### Autocorrect Engine
- Backed by an embedded `fzf` library; receives the current scope symbol list and the unknown identifier, returns ranked suggestions.
- During `run`, unknown symbol triggers interactive prompt; on `build`, can auto-apply highest-confidence fix when allowed.

### Import Resolver
- `@import c/...` uses libclang to parse system headers and produce binding stubs.
- `@import c++/...` relies on clang's AST to expose classes/functions with name mangling preserved.

### Diagnostics & Tooling
- All warnings/errors reference source spans; RAII injections should surface as notes for debuggability.
- Language server hooks share the same AST/semantic representation to provide completions.

This memo informs the implementation TODOs and will expand as subsystems solidify.
