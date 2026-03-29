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
- [v] Implement semantic type representation (`margo_type_t`) with full scalar set including `uint8_t`/`uint64_t` families and `size_t`.
- [v] `weird` dimension validation (error on dimension outside [0, 8]).
- [v] Function signature registry with call-site arity checking.

## Memory Management
- [v] Design the RAII ownership tracker that injects `free()` on block exit for `alloc`/`alloc_and_init` results.
- [v] Describe ownership-transfer semantics when identifiers are reassigned or leave scope.
- [v] Implement RAII scope-exit free injection in the transpiler (scope-close path).
- [v] Implement RAII return-path free injection with ownership-transfer detection.

## Control Flow
- [v] Define parser desugaring for implicit loop variables (`for x=0; <N; ++`).
- [v] Model conditional increment/ternary expressions within the loop increment slot.

## Directives & FFI
- [v] Map directive handling (`@import`, `@style`, `@use_switch_optim`, `@set autocorrect`) to compiler phases and capability flags.
- [v] Outline header/C++ parser integration strategy for `@import c/` and `@import c++/`.
- [v] Implement `@import godmode` expanding to all standard C headers.
- [v] Implement `@import c++/...` stub (preserved as comment, full C++ binding is future work).
- [v] Implement extended `@import std/...` modules: `string`, `math`, `stdlib`, `time`, `assert`, `errno`.

## Keywords
- [v] Implement `null` keyword lowering to C `NULL`.

## Semantic Analysis Pass (sema.c)
- [v] Symbol table with scope-depth tracking.
- [v] `sema_parse_type` for all Margo type annotations.
- [v] Multi-error diagnostic accumulation (`sema_diag_list_t`) with IDE-compatible output.
- [v] `weird` pointer dimension validation.
- [v] Function call arity checking against registered signatures.
- [v] RAII ownership table built from alloc-assignment pattern detection.

## Integrated Tools
- [ ] Prototype the fzf-powered autocorrect suggestion flow and tie it to the identifier resolution pass.

## LibTTAK Integration Gaps
- [v] Replace the current `ttak_mem_alloc(...__TTAK_UNSAFE_MEM_FOREVER__)` fallback path with real detachable arenas + generation resets so `alloc`/`alloc_and_init` scopes exercise libttak's generational ownership flow (see `ttak_mem_alloc_safe`, arena capability flags).
- [v] Bring up a per-process `ttak_epoch_manager_t`, register worker threads, and route delayed frees via `ttak_epoch_retire` so cross-thread objects honor libttak's three-session epoch reclamation contract.
- [ ] Surface `ttak_owner_t` / context-bridge policies inside runtime helpers (IO, network) so user callbacks execute under owner-enforced guardrails instead of bypassing libttak's safe execution layer.
- [ ] Integrate segmented shard tables + the built-in thread pool (`ttak_thread_pool_t`, `ttak_shard_table_t`) to back future async runtime services, rather than relying on ad-hoc pthread or single-thread fallbacks.
- [ ] Expose libttak observability/acceleration toggles (`ttak_mem_set_trace`, `ttak_execute_batch`, security engine drivers) through CLI flags so transpiled programs can opt into tracing, accelerator selection, and integrity enforcement.
