# Margo Grammar Reference

아래 항목은 현재 저장소에서 구현/문서화된 핵심 문법 축입니다.

## Imports and directives

- `@import c/<header>`
- `@import std/<module>`
- `@import godmode`
- `@import threads/core`, `@import process`, `@import matrix/core`
- `@style`, `@set`, decorator-like `@name`

## Function and type sugar

- `fn [return_type] name(args) { ... }`
- `string` alias
- `auto` inference
- `weird(T, n)` pointer shorthand
- `null` keyword

## Control flow sugar

- compact `if` / `while` header without explicit parentheses
- compact `for` header with implicit loop identifier completion
- `for x in [ ... ]` list-iteration lowering

## Matrix DSL

- `mat_for(matrix, fn(...){...})`
- `mat_find(matrix, target, precision) { ... }`
- `mat_neighbor(...)`

정확한 동작은 `src/lexer.*`, `src/parser.*`, `src/transpiler.*`, `src/sema.*`의 API/구현 문서를 함께 참고하세요.
