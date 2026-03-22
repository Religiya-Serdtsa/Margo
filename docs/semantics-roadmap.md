# Semantic Strategy Notes

## Memory Management
- **RAII Tracker**: `ScopeTracker` owns a stack of scopes; each `alloc`/`alloc_and_init` registers an `AllocationHandle`. Upon scope exit the tracker emits a synthesized `free(handle.symbol)` IR call.
- **Ownership Transfer**: Assignments between identifiers move the `AllocationHandle` entry. Reassigning an owning variable first emits `free` before capturing the new handle.
- **Escape Analysis**: Handles escaping via return statements or pointer leaks mark the allocation as transferred, skipping automatic frees.

## Control Flow
- **Implicit Loop Variables**: Parser creates a hidden `VarDecl` node scoped to the `for` loop body. The code generator treats `for x=0; <10; ++` as sugar for `for (int x=0; x<10; ++x)`.
- **Conditional Increment Slot**: Increment expression is parsed as a general expression AST node so constructs like `count.odd? ++ : +=2` can be compiled into conditional IR blocks.

## Directives & FFI
- **Directive Table**: During parsing, each `@directive` registers a handler that may mutate parser state (`@style c`), import bindings (`@import c/...`) or set capability flags (`@import godmode`).
- **C/C++ Imports**: Use libclang to parse headers/classes and store them as foreign symbols referencing the exact ABI name. The compiler ensures `weird` pointer shapes match the imported signatures.
- **Autocorrect Toggle**: Directive `@set autocorrect on/off` flips a flag consumed by the identifier-resolution pass; the pass consults the fzf-backed suggester when enabled.
