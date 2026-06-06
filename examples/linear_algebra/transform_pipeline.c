#define alloc malloc
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../../runtime/include/margo_matrix/core.h"

static void print_matrix(auto_matrix m, const char *name) {
    printf("%s:\n", name);
    for (size_t r = 0; r < matrix_rows(m); ++r) {
        for (size_t c = 0; c < matrix_cols(m); ++c) {
            printf("%8.4f ", matrix_get(m, r, c));
        }
        printf("\n");
    }
}

static auto_matrix translation(double tx, double ty, double tz) {
    auto_matrix m = matrix_identity(4);
    matrix_set(m, 0, 3, tx);
    matrix_set(m, 1, 3, ty);
    matrix_set(m, 2, 3, tz);
    return m;
}

static auto_matrix rotation_z(double angle) {
    auto_matrix m = matrix_identity(4);
    double c = cos(angle);
    double s = sin(angle);
    matrix_set(m, 0, 0, c);
    matrix_set(m, 0, 1, -s);
    matrix_set(m, 1, 0, s);
    matrix_set(m, 1, 1, c);
    return m;
}

static auto_matrix scaling(double sx, double sy, double sz) {
    auto_matrix m = matrix_identity(4);
    matrix_set(m, 0, 0, sx);
    matrix_set(m, 1, 1, sy);
    matrix_set(m, 2, 2, sz);
    return m;
}

int main(void) {
    auto_matrix T = translation(1.0, 2.0, 3.0);
    auto_matrix R = rotation_z(3.14159 / 4.0);
    auto_matrix S = scaling(2.0, 2.0, 2.0);

    auto_matrix TR  = matrix_mul(T, R);
    auto_matrix TRS = matrix_mul(TR, S);

    print_matrix(T,   "Translation (1,2,3)");
    print_matrix(R,   "Rotation Z (45 deg)");
    print_matrix(S,   "Scaling (2,2,2)");
    print_matrix(TR,  "T * R");
    print_matrix(TRS, "(T * R) * S");

    auto_matrix v = matrix_alloc(4, 1);
    matrix_set(v, 0, 0, 1.0);
    matrix_set(v, 1, 0, 0.0);
    matrix_set(v, 2, 0, 0.0);
    matrix_set(v, 3, 0, 1.0);

    auto_matrix Tv   = matrix_mul(T, v);
    auto_matrix RTv  = matrix_mul(R, Tv);
    auto_matrix SRTv = matrix_mul(S, RTv);

    print_matrix(v,     "Vertex (1,0,0,1)");
    print_matrix(Tv,    "After translation");
    print_matrix(RTv,   "After rotation");
    print_matrix(SRTv,  "After scaling");

    free(T);
    free(R);
    free(S);
    free(TR);
    free(TRS);
    free(v);
    free(Tv);
    free(RTv);
    free(SRTv);
    return 0;
}
