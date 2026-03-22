# Spec Outline (Implementation View)

## 1. Philosophy
- Preserve raw C layouts to avoid marshalling; compiler must default to C ABI for every exposed function.
- Enforce RAII semantics at compile time to satisfy "Zero-Grief" memory handling.
- Keep frontend fast (interpreter-like dev loop) while generating LLVM IR for production builds.

## 2. Type System
- Scalar set mirrors C: `int`, `char`, `float`, `double`, plus `void` for explicit lack of return values.
- `string` aliases `char *` but the allocator/runtime appends and tracks `\0` automatically.
- `auto` triggers inference based on the initializer expression; inference happens after semantic analysis so pointer decay is visible.
- `weird(T, n)` lowers to `T` with `n` pointer stars; compiler should store dimension metadata for diagnostics.

## 3. Memory Management
- Built-in `alloc` / `alloc_and_init` produce ownership-bound handles; the compiler injects `free` at block exit.
- RAII tracking ties identifiers to scopes; reassigning should either transfer ownership or re-register the symbol.

## 4. Control Flow
- `for x=0; <N; ++ { ... }`: the unnamed loop variable (`x` here) is implicit and scoped to the loop body.
- `if v == 1 return 100`: parenthesis-free headers get wrapped during parsing so `return`/`break`/`continue`-style statements keep concept syntax while the emitted C receives `if (v == 1)`.
- Increment slot accepts expression-like statements, enabling `count.odd? ++ : +=2` semantics.

## 5. Directives & FFI
- `@import` prefixes (`c/`, `c++/`, `godmode`) define parsing paths and capability flags.
- `@style c { ... }` toggles strict C-like parsing for easy code importing.
- Decorators such as `@use_switch_optim` hint optimizer passes.

## 6. Integrated Tools
- `@set autocorrect on` delegates unknown identifier resolution to an fzf-backed suggestion engine during compilation.

This outline feeds directly into the actionable roadmap captured in `TODO.md`.
