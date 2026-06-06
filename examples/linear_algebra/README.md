# Linear Algebra Examples: C vs Margo

This directory demonstrates how Margo's `@import matrix/core` and built-in
syntax sugar shrink the gap between high-level intent and low-level C.

Each topic has two files:
- `*.margo` — Margo version (RAII, auto-free, paren-free syntax)
- `*.c`     — equivalent C version (manual malloc/free, verbose types)

---

## `chain_multiply`

Creates three matrices, multiplies them in sequence, then transposes the result.

| Aspect | C | Margo |
|---|---|---|
| Allocation | `matrix_fill(...)` + manual `free()` for every temp | `auto_matrix` created by `matrix_fill`; temps freed automatically by RAII |
| Chaining | Must name every intermediate (`AB`, `ABC`) | Same, but no `free` bookkeeping |
| Transpose | `matrix_transpose(ABC)` | identical API |

---

## `gaussian_elimination`

Solves a 3×3 linear system with Gaussian elimination + back-substitution.

| Aspect | C | Margo |
|---|---|---|
| Loop style | `for (size_t i = 0; i < n; ++i)` | `for i = 0; <n; ++` |
| Back-sub loop | `for (size_t idx = n; idx > 0; --idx)` | `for i = n; >0; --` |
| Matrix access | `matrix_get(aug, r, c)` / `matrix_set(...)` | identical API |
| Memory | `free(aug); free(x);` | automatic |

---

## `convolution`

Applies a 3×3 Sobel-X kernel to a 5×5 image with zero-padding.

| Aspect | C | Margo |
|---|---|---|
| Kernel type | `double kernel[3][3]` | `double kernel[3][3]` (same) |
| Boundary check | explicit `sr >= 0 && sr < rows` | identical logic |
| Key win | still need `free(img); free(edge);` | no manual cleanup |

---

## `transform_pipeline`

Builds a classic 3-D graphics pipeline: **Translation → Rotation → Scaling**,
then applies it step-by-step to a vertex vector.

| Aspect | C | Margo |
|---|---|---|
| Identity matrix | `matrix_identity(4)` | identical |
| Pipeline composition | `matrix_mul(T, R)`, `matrix_mul(TR, S)` | identical |
| Vertex transform | manual chain of `matrix_mul` calls | identical |
| Cleanup | 9 separate `free()` calls | automatic |

---

## Build

```bash
# Margo examples
margo-c build chain_multiply.margo          -o chain_multiply
margo-c build gaussian_elimination.margo    -o gaussian_elimination
margo-c build convolution.margo             -o convolution
margo-c build transform_pipeline.margo      -o transform_pipeline

# C examples (needs margo_matrix headers)
clang -I../../runtime/include chain_multiply.c        -o chain_multiply_c -lm
clang -I../../runtime/include gaussian_elimination.c  -o gaussian_elimination_c -lm
clang -I../../runtime/include convolution.c           -o convolution_c -lm
clang -I../../runtime/include transform_pipeline.c    -o transform_pipeline_c -lm
```
