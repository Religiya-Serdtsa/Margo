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

static auto_matrix convolve(auto_matrix src, double kernel[3][3]) {
    size_t rows = matrix_rows(src);
    size_t cols = matrix_cols(src);
    auto_matrix out = matrix_fill(rows, cols, 0.0);
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            double sum = 0.0;
            for (size_t kr = 0; kr < 3; ++kr) {
                for (size_t kc = 0; kc < 3; ++kc) {
                    int sr = (int)r + (int)kr - 1;
                    int sc = (int)c + (int)kc - 1;
                    if (sr >= 0 && sr < (int)rows && sc >= 0 && sc < (int)cols) {
                        sum += matrix_get(src, (size_t)sr, (size_t)sc) * kernel[kr][kc];
                    }
                }
            }
            matrix_set(out, r, c, sum);
        }
    }
    return out;
}

int main(void) {
    auto_matrix img = matrix_alloc(5, 5);
    for (size_t r = 0; r < 5; ++r) {
        for (size_t c = 0; c < 5; ++c) {
            matrix_set(img, r, c, (double)(r * 5 + c));
        }
    }

    double sobel_x[3][3] = {
        {-1, 0, 1},
        {-2, 0, 2},
        {-1, 0, 1},
    };

    auto_matrix edge = convolve(img, sobel_x);

    print_matrix(img,  "Original 5x5 gradient");
    print_matrix(edge, "Sobel-X edge response");

    free(img);
    free(edge);
    return 0;
}
