#define alloc malloc
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../../runtime/include/margo_matrix/core.h"

static void print_matrix(auto_matrix m, const char *name) {
    printf("%s:\n", name);
    for (size_t r = 0; r < matrix_rows(m); ++r) {
        for (size_t c = 0; c < matrix_cols(m); ++c) {
            printf("%8.3f ", matrix_get(m, r, c));
        }
        printf("\n");
    }
}

static void gaussian_eliminate(auto_matrix aug, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        double pivot = matrix_get(aug, i, i);
        if (pivot == 0.0) {
            continue;
        }
        for (size_t j = i; j < n + 1; ++j) {
            matrix_set(aug, i, j, matrix_get(aug, i, j) / pivot);
        }
        for (size_t k = i + 1; k < n; ++k) {
            double factor = matrix_get(aug, k, i);
            for (size_t j = i; j < n + 1; ++j) {
                double val = matrix_get(aug, k, j) - factor * matrix_get(aug, i, j);
                matrix_set(aug, k, j, val);
            }
        }
    }
}

static void back_substitute(auto_matrix aug, size_t n, auto_matrix x) {
    for (size_t idx = n; idx > 0; --idx) {
        size_t i = idx - 1;
        double sum = matrix_get(aug, i, n);
        for (size_t j = i + 1; j < n; ++j) {
            sum -= matrix_get(aug, i, j) * matrix_get(x, j, 0);
        }
        matrix_set(x, i, 0, sum / matrix_get(aug, i, i));
    }
}

int main(void) {
    auto_matrix aug = matrix_alloc(3, 4);
    matrix_set(aug, 0, 0, 2.0);  matrix_set(aug, 0, 1, 1.0);  matrix_set(aug, 0, 2, -1.0); matrix_set(aug, 0, 3, 8.0);
    matrix_set(aug, 1, 0, -3.0); matrix_set(aug, 1, 1, -1.0); matrix_set(aug, 1, 2, 2.0);  matrix_set(aug, 1, 3, -11.0);
    matrix_set(aug, 2, 0, -2.0); matrix_set(aug, 2, 1, 1.0);  matrix_set(aug, 2, 2, 2.0);  matrix_set(aug, 2, 3, -3.0);

    auto_matrix x = matrix_alloc(3, 1);

    print_matrix(aug, "Augmented matrix [A|b]");
    gaussian_eliminate(aug, 3);
    print_matrix(aug, "After Gaussian elimination");
    back_substitute(aug, 3, x);
    print_matrix(x, "Solution x");

    free(aug);
    free(x);
    return 0;
}
