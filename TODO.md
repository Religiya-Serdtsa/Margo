# TODO (Margo v0.1)

## Documentation & Planning
- [v] Draft bilingual README files summarizing the concept spec and repository layout.
- [v] Translate `concept.margo` into an implementation-facing outline in `docs/spec-outline.md`.
- [v] Flesh out a compiler architecture memo (frontend, semantic passes, LLVM backend integration).

## Compiler Prototype
- [v] Bootstrap a C-based `margo` CLI that parses declarations, performs `auto` inference, and lowers `weird(T, n)` pointers using libttak arenas.
- [v] Add transpile/build commands that convert `fn`/`@import`/`auto`/`weird` constructs into ANSI C and invoke the host compiler.

## Type System
- [v] Specify syntax + AST nodes for core types (`int`, `char`, `float`, `double`, `void`, `string`).
- [v] Formalize `auto` inference rules and `weird(T, n)` lowering logic.

## Memory Management
- [v] Design the RAII ownership tracker that injects `free()` on block exit for `alloc`/`alloc_and_init` results.
- [v] Describe ownership-transfer semantics when identifiers are reassigned or leave scope.

## Control Flow
- [v] Define parser desugaring for implicit loop variables (`for x=0; <N; ++`).
- [v] Model conditional increment/ternary expressions within the loop increment slot.

## Directives & FFI
- [v] Map directive handling (`@import`, `@style`, `@use_switch_optim`, `@set autocorrect`) to compiler phases and capability flags.
- [v] Outline header/C++ parser integration strategy for `@import c/` and `@import c++/`.

## Integrated Tools
- [ ] Prototype the fzf-powered autocorrect suggestion flow and tie it to the identifier resolution pass.
