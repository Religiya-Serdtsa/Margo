#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct margo_matrix {
    size_t rows;
    size_t cols;
    double data[];
} margo_matrix;

typedef margo_matrix *auto_matrix;

typedef struct {
    size_t rank;
    size_t index[4];
    size_t distance;
    double value;
} mat_neighbor_cell;

typedef bool (*matrix_iter_fn)(size_t row, size_t col, double value, void *ctx);
typedef bool (*neighbor_filter_fn)(mat_neighbor_cell cell, void *ctx);
typedef bool (*neighbor_visit_fn)(mat_neighbor_cell cell, void *ctx);

typedef struct {
    auto_matrix matrix;
    size_t row;
    size_t col;
    size_t radius;
    neighbor_filter_fn filter;
    void *filter_ctx;
} neighbor_view;

static inline size_t matrix_rows(auto_matrix matrix) {
    return matrix ? matrix->rows : 0;
}

static inline size_t matrix_cols(auto_matrix matrix) {
    return matrix ? matrix->cols : 0;
}

static inline size_t matrix_linear_index(auto_matrix matrix, size_t row, size_t col) {
    if (!matrix || row >= matrix->rows || col >= matrix->cols) {
        return 0;
    }
    return row * matrix->cols + col;
}

static inline double matrix_get(auto_matrix matrix, size_t row, size_t col) {
    if (!matrix || row >= matrix->rows || col >= matrix->cols) {
        return 0.0;
    }
    return matrix->data[matrix_linear_index(matrix, row, col)];
}

static inline void matrix_set(auto_matrix matrix, size_t row, size_t col, double value) {
    if (!matrix || row >= matrix->rows || col >= matrix->cols) {
        return;
    }
    matrix->data[matrix_linear_index(matrix, row, col)] = value;
}

static inline double *matrix_data(auto_matrix matrix) {
    return matrix ? matrix->data : NULL;
}

static inline const double *matrix_data_const(auto_matrix matrix) {
    return matrix ? matrix->data : NULL;
}

static inline double *matrix_row_ptr(auto_matrix matrix, size_t row) {
    if (!matrix || row >= matrix->rows) {
        return NULL;
    }
    return &matrix->data[row * matrix->cols];
}

static inline const double *matrix_row_ptr_const(auto_matrix matrix, size_t row) {
    if (!matrix || row >= matrix->rows) {
        return NULL;
    }
    return &matrix->data[row * matrix->cols];
}

static inline auto_matrix matrix_alloc(size_t rows, size_t cols) {
    if (!rows || !cols) {
        return NULL;
    }
    size_t total = rows * cols;
    auto_matrix matrix = alloc(sizeof(margo_matrix) + total * sizeof(double));
    if (!matrix) {
        return NULL;
    }
    matrix->rows = rows;
    matrix->cols = cols;
    return matrix;
}

static inline auto_matrix matrix_fill(size_t rows, size_t cols, double value) {
    auto_matrix matrix = matrix_alloc(rows, cols);
    if (!matrix) {
        return NULL;
    }
    size_t total = rows * cols;
    for (size_t i = 0; i < total; ++i) {
        matrix->data[i] = value;
    }
    return matrix;
}

static inline auto_matrix matrix_clone(auto_matrix source) {
    if (!source) {
        return NULL;
    }
    size_t total = source->rows * source->cols;
    auto_matrix copy = matrix_fill(source->rows, source->cols, 0.0);
    if (!copy) {
        return NULL;
    }
    for (size_t i = 0; i < total; ++i) {
        copy->data[i] = source->data[i];
    }
    return copy;
}

static inline auto_matrix matrix_identity_with_value(size_t size, double diag_value) {
    auto_matrix matrix = matrix_fill(size, size, 0.0);
    if (!matrix) {
        return NULL;
    }
    for (size_t i = 0; i < size; ++i) {
        matrix_set(matrix, i, i, diag_value);
    }
    return matrix;
}

static inline auto_matrix matrix_identity_default(size_t size) {
    return matrix_identity_with_value(size, 1.0);
}

#define MARGO_MATRIX_IDENTITY_SELECT(_1, _2, NAME, ...) NAME
#define matrix_identity(...) \
    MARGO_MATRIX_IDENTITY_SELECT(__VA_ARGS__, matrix_identity_with_value, matrix_identity_default)(__VA_ARGS__)

static inline auto_matrix matrix_transpose(auto_matrix matrix) {
    if (!matrix) {
        return NULL;
    }
    auto_matrix out = matrix_fill(matrix->cols, matrix->rows, 0.0);
    if (!out) {
        return NULL;
    }
    for (size_t r = 0; r < matrix->rows; ++r) {
        for (size_t c = 0; c < matrix->cols; ++c) {
            matrix_set(out, c, r, matrix_get(matrix, r, c));
        }
    }
    return out;
}

static inline auto_matrix matrix_mul(auto_matrix lhs, auto_matrix rhs) {
    if (!lhs || !rhs || matrix_cols(lhs) != matrix_rows(rhs)) {
        return NULL;
    }
    auto_matrix out = matrix_fill(matrix_rows(lhs), matrix_cols(rhs), 0.0);
    if (!out) {
        return NULL;
    }
    for (size_t r = 0; r < matrix_rows(lhs); ++r) {
        for (size_t c = 0; c < matrix_cols(rhs); ++c) {
            double acc = 0.0;
            for (size_t k = 0; k < matrix_cols(lhs); ++k) {
                acc += matrix_get(lhs, r, k) * matrix_get(rhs, k, c);
            }
            matrix_set(out, r, c, acc);
        }
    }
    return out;
}

static inline void matrix_for_each(auto_matrix matrix, matrix_iter_fn fn, void *ctx) {
    if (!matrix || !fn) {
        return;
    }
    for (size_t r = 0; r < matrix->rows; ++r) {
        for (size_t c = 0; c < matrix->cols; ++c) {
            if (!fn(r, c, matrix_get(matrix, r, c), ctx)) {
                return;
            }
        }
    }
}

static inline neighbor_view matrix_neighbor_with_filter(auto_matrix matrix,
                                                        size_t row,
                                                        size_t col,
                                                        size_t radius,
                                                        neighbor_filter_fn filter,
                                                        void *filter_ctx) {
    neighbor_view view = {
        .matrix = matrix,
        .row = row,
        .col = col,
        .radius = radius ? radius : 1,
        .filter = filter,
        .filter_ctx = filter_ctx,
    };
    return view;
}

static inline neighbor_view matrix_neighbor(auto_matrix matrix,
                                            size_t row,
                                            size_t col,
                                            size_t radius) {
    return matrix_neighbor_with_filter(matrix, row, col, radius, NULL, NULL);
}

static inline size_t neighbor_view_count(const neighbor_view *view) {
    if (!view || !view->matrix) {
        return 0;
    }
    size_t rows = matrix_rows(view->matrix);
    size_t cols = matrix_cols(view->matrix);
    if (!rows || !cols) {
        return 0;
    }
    size_t count = 0;
    mat_neighbor_cell cell;
    cell.rank = 2;
    for (size_t r = (view->row > view->radius ? view->row - view->radius : 0);
         r <= view->row + view->radius && r < rows;
         ++r) {
        for (size_t c = (view->col > view->radius ? view->col - view->radius : 0);
             c <= view->col + view->radius && c < cols;
             ++c) {
            if (r == view->row && c == view->col) {
                continue;
            }
            size_t dr = (r > view->row) ? (r - view->row) : (view->row - r);
            size_t dc = (c > view->col) ? (c - view->col) : (view->col - c);
            cell.index[0] = r;
            cell.index[1] = c;
            cell.distance = dr > dc ? dr : dc;
            cell.value = matrix_get(view->matrix, r, c);
            if (view->filter && !view->filter(cell, view->filter_ctx)) {
                continue;
            }
            count++;
        }
    }
    return count;
}

static inline bool neighbor_view_for_each(const neighbor_view *view,
                                          neighbor_visit_fn visit,
                                          void *ctx) {
    if (!view || !view->matrix || !visit) {
        return false;
    }
    size_t max_rows = matrix_rows(view->matrix);
    size_t max_cols = matrix_cols(view->matrix);
    if (!max_rows || !max_cols) {
        return true;
    }
    size_t start_row = (view->row > view->radius) ? view->row - view->radius : 0;
    size_t start_col = (view->col > view->radius) ? view->col - view->radius : 0;
    size_t end_row = view->row + view->radius;
    size_t end_col = view->col + view->radius;
    if (end_row >= max_rows) {
        end_row = max_rows ? max_rows - 1 : 0;
    }
    if (end_col >= max_cols) {
        end_col = max_cols ? max_cols - 1 : 0;
    }
    mat_neighbor_cell cell;
    cell.rank = 2;
    for (size_t r = start_row; r <= end_row && r < max_rows; ++r) {
        for (size_t c = start_col; c <= end_col && c < max_cols; ++c) {
            if (r == view->row && c == view->col) {
                continue;
            }
            size_t dr = (r > view->row) ? (r - view->row) : (view->row - r);
            size_t dc = (c > view->col) ? (c - view->col) : (view->col - c);
            cell.index[0] = r;
            cell.index[1] = c;
            cell.distance = dr > dc ? dr : dc;
            cell.value = matrix_get(view->matrix, r, c);
            if (view->filter && !view->filter(cell, view->filter_ctx)) {
                continue;
            }
            if (!visit(cell, ctx)) {
                return false;
            }
        }
    }
    return true;
}

typedef double (*margo_matrix_reader_fn)(const void *base, size_t index);

#define MARGO_MATRIX_DEFINE_READER(type, name)                     \
    static inline double name(const void *base, size_t index) {    \
        const type *typed = (const type *)base;                    \
        return (double)typed[index];                               \
    }

MARGO_MATRIX_DEFINE_READER(signed char, margo_matrix_read_schar)
MARGO_MATRIX_DEFINE_READER(unsigned char, margo_matrix_read_uchar)
MARGO_MATRIX_DEFINE_READER(short, margo_matrix_read_short)
MARGO_MATRIX_DEFINE_READER(unsigned short, margo_matrix_read_ushort)
MARGO_MATRIX_DEFINE_READER(int, margo_matrix_read_int)
MARGO_MATRIX_DEFINE_READER(unsigned int, margo_matrix_read_uint)
MARGO_MATRIX_DEFINE_READER(long, margo_matrix_read_long)
MARGO_MATRIX_DEFINE_READER(unsigned long, margo_matrix_read_ulong)
MARGO_MATRIX_DEFINE_READER(long long, margo_matrix_read_llong)
MARGO_MATRIX_DEFINE_READER(unsigned long long, margo_matrix_read_ullong)
MARGO_MATRIX_DEFINE_READER(float, margo_matrix_read_float)
MARGO_MATRIX_DEFINE_READER(double, margo_matrix_read_double)

#undef MARGO_MATRIX_DEFINE_READER

#define MARGO_MATRIX_SAMPLE(array_expr) ((__typeof__((array_expr)[0][0]))0)
#define MARGO_MATRIX_PICK_READER(array_expr)                                      \
    _Generic(MARGO_MATRIX_SAMPLE(array_expr),                                     \
             signed char: margo_matrix_read_schar,                                \
             unsigned char: margo_matrix_read_uchar,                              \
             short: margo_matrix_read_short,                                      \
             unsigned short: margo_matrix_read_ushort,                            \
             int: margo_matrix_read_int,                                          \
             unsigned int: margo_matrix_read_uint,                                \
             long: margo_matrix_read_long,                                        \
             unsigned long: margo_matrix_read_ulong,                              \
             long long: margo_matrix_read_llong,                                  \
             unsigned long long: margo_matrix_read_ullong,                        \
             float: margo_matrix_read_float,                                      \
             double: margo_matrix_read_double,                                    \
             default: margo_matrix_read_double)

static inline auto_matrix margo_matrix_map_from_array_impl(const void *base,
                                                           size_t rows,
                                                           size_t cols,
                                                           margo_matrix_reader_fn reader,
                                                           double (*mapper)(mat_neighbor_cell)) {
    if (!base || !rows || !cols || !reader || !mapper) {
        return NULL;
    }
    auto_matrix out = matrix_fill(rows, cols, 0.0);
    if (!out) {
        return NULL;
    }
    mat_neighbor_cell cell;
    cell.rank = 2;
    cell.distance = 0;
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            size_t idx = r * cols + c;
            cell.index[0] = r;
            cell.index[1] = c;
            cell.value = reader(base, idx);
            matrix_set(out, r, c, mapper(cell));
        }
    }
    return out;
}

#define matrix_map(array_expr, mapper_fn)                                                   \
    margo_matrix_map_from_array_impl(&(array_expr)[0][0],                                   \
                                     sizeof(array_expr) / sizeof((array_expr)[0]),          \
                                     sizeof((array_expr)[0]) / sizeof((array_expr)[0][0]),  \
                                     MARGO_MATRIX_PICK_READER(array_expr),                 \
                                     (mapper_fn))

#ifdef __cplusplus
}
#endif
