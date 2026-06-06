#define alloc malloc
#include <stdio.h>
#include <stdlib.h>
#include "../../runtime/include/margo_matrix/core.h"

static void print_matrix(auto_matrix m, const char *name) {
    printf("%s:\n", name);
    for (size_t r = 0; r < matrix_rows(m); ++r) {
        for (size_t c = 0; c < matrix_cols(m); ++c) {
            printf("%6.1f ", matrix_get(m, r, c));
        }
        printf("\n");
    }
}

int main(void) {
    auto_matrix A = matrix_fill(2, 3, 1.0);
    auto_matrix B = matrix_fill(3, 4, 2.0);
    auto_matrix C = matrix_fill(4, 2, 3.0);

    auto_matrix AB  = matrix_mul(A, B);
    auto_matrix ABC = matrix_mul(AB, C);
    auto_matrix T   = matrix_transpose(ABC);

    print_matrix(A,   "A (2x3, filled with 1)");
    print_matrix(B,   "B (3x4, filled with 2)");
    print_matrix(C,   "C (4x2, filled with 3)");
    print_matrix(AB,  "A * B");
    print_matrix(ABC, "(A * B) * C");
    print_matrix(T,   "Transpose of result");

    free(A);
    free(B);
    free(C);
    free(AB);
    free(ABC);
    free(T);
    return 0;
}
