#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

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

typedef struct {
    double *data;
    double value;
    size_t begin;
    size_t end;
} matrix_fill_job;

typedef struct {
    auto_matrix lhs;
    auto_matrix rhs;
    auto_matrix out;
    size_t row_begin;
    size_t row_end;
} matrix_mul_job;

static inline void *matrix_fill_worker(void *arg) {
    matrix_fill_job *job = (matrix_fill_job *)arg;
    if (job->value == 0.0) {
        memset(job->data + job->begin, 0, (job->end - job->begin) * sizeof(double));
    } else {
        for (size_t i = job->begin; i < job->end; ++i) {
            job->data[i] = job->value;
        }
    }
    return NULL;
}

static inline void matrix_mul_worker_block(const matrix_mul_job *job) {
    size_t lhs_cols = job->lhs->cols;
    size_t rhs_cols = job->rhs->cols;
    for (size_t r = job->row_begin; r < job->row_end; ++r) {
        size_t lhs_base = r * lhs_cols;
        size_t out_base = r * rhs_cols;
        for (size_t c = 0; c < rhs_cols; ++c) {
            double acc = 0.0;
            for (size_t k = 0; k < lhs_cols; ++k) {
                acc += job->lhs->data[lhs_base + k] * job->rhs->data[k * rhs_cols + c];
            }
            job->out->data[out_base + c] = acc;
        }
    }
}

static inline void *matrix_mul_worker(void *arg) {
    const matrix_mul_job *job = (const matrix_mul_job *)arg;
    matrix_mul_worker_block(job);
    return NULL;
}

static inline size_t margo_matrix_hw_threads(void) {
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count < 1) {
        return 1;
    }
    return (size_t)cpu_count;
}

static inline size_t margo_matrix_auto_threads(size_t work_items) {
    if (work_items < 65536) {
        return 1;
    }
    const char *env_value = getenv("MARGO_MATRIX_THREADS");
    size_t max_threads = margo_matrix_hw_threads();
    if (env_value && env_value[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(env_value, &end, 10);
        if (end != env_value && *end == '\0' && parsed > 0) {
            max_threads = (size_t)parsed;
        }
    }
    if (max_threads < 2) {
        return 1;
    }
    size_t suggested = work_items / 16384;
    if (suggested < 1) {
        suggested = 1;
    }
    if (suggested > max_threads) {
        suggested = max_threads;
    }
    return suggested;
}

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

static inline size_t matrix_linear_index_unchecked(auto_matrix matrix, size_t row, size_t col) {
    return row * matrix->cols + col;
}

static inline double matrix_get(auto_matrix matrix, size_t row, size_t col) {
    if (!matrix || row >= matrix->rows || col >= matrix->cols) {
        return 0.0;
    }
    return matrix->data[matrix_linear_index_unchecked(matrix, row, col)];
}

static inline void matrix_set(auto_matrix matrix, size_t row, size_t col, double value) {
    if (!matrix || row >= matrix->rows || col >= matrix->cols) {
        return;
    }
    matrix->data[matrix_linear_index_unchecked(matrix, row, col)] = value;
}

static inline auto_matrix matrix_fill(size_t rows, size_t cols, double value) {
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
    size_t thread_count = margo_matrix_auto_threads(total);
    if (thread_count <= 1) {
        if (value == 0.0) {
            memset(matrix->data, 0, total * sizeof(double));
        } else {
            for (size_t i = 0; i < total; ++i) {
                matrix->data[i] = value;
            }
        }
        return matrix;
    }

    pthread_t *threads = (pthread_t *)alloc(thread_count * sizeof(pthread_t));
    matrix_fill_job *jobs = (matrix_fill_job *)alloc(thread_count * sizeof(matrix_fill_job));
    if (!threads || !jobs) {
        if (value == 0.0) {
            memset(matrix->data, 0, total * sizeof(double));
        } else {
            for (size_t i = 0; i < total; ++i) {
                matrix->data[i] = value;
            }
        }
        return matrix;
    }

    size_t base_chunk = total / thread_count;
    size_t extra = total % thread_count;
    size_t cursor = 0;
    for (size_t t = 0; t < thread_count; ++t) {
        size_t span = base_chunk + (t < extra ? 1 : 0);
        jobs[t].data = matrix->data;
        jobs[t].value = value;
        jobs[t].begin = cursor;
        jobs[t].end = cursor + span;
        cursor += span;
        if (pthread_create(&threads[t], NULL, matrix_fill_worker, &jobs[t]) != 0) {
            for (size_t joined = 0; joined < t; ++joined) {
                pthread_join(threads[joined], NULL);
            }
            if (value == 0.0) {
                memset(matrix->data, 0, total * sizeof(double));
            } else {
                for (size_t i = 0; i < total; ++i) {
                    matrix->data[i] = value;
                }
            }
            return matrix;
        }
    }

    for (size_t t = 0; t < thread_count; ++t) {
        pthread_join(threads[t], NULL);
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
        size_t src_base = r * matrix->cols;
        for (size_t c = 0; c < matrix->cols; ++c) {
            out->data[c * out->cols + r] = matrix->data[src_base + c];
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
    size_t lhs_rows = lhs->rows;
    size_t thread_count = margo_matrix_auto_threads(lhs_rows * rhs->cols * lhs->cols);
    if (thread_count <= 1 || lhs_rows < 2) {
        matrix_mul_job single = { lhs, rhs, out, 0, lhs_rows };
        matrix_mul_worker_block(&single);
        return out;
    }

    if (thread_count > lhs_rows) {
        thread_count = lhs_rows;
    }
    pthread_t *threads = (pthread_t *)alloc(thread_count * sizeof(pthread_t));
    matrix_mul_job *jobs = (matrix_mul_job *)alloc(thread_count * sizeof(matrix_mul_job));
    if (!threads || !jobs) {
        matrix_mul_job single = { lhs, rhs, out, 0, lhs_rows };
        matrix_mul_worker_block(&single);
        return out;
    }

    size_t base_rows = lhs_rows / thread_count;
    size_t extra_rows = lhs_rows % thread_count;
    size_t cursor = 0;
    for (size_t t = 0; t < thread_count; ++t) {
        size_t span = base_rows + (t < extra_rows ? 1 : 0);
        jobs[t].lhs = lhs;
        jobs[t].rhs = rhs;
        jobs[t].out = out;
        jobs[t].row_begin = cursor;
        jobs[t].row_end = cursor + span;
        cursor += span;
        if (pthread_create(&threads[t], NULL, matrix_mul_worker, &jobs[t]) != 0) {
            for (size_t joined = 0; joined < t; ++joined) {
                pthread_join(threads[joined], NULL);
            }
            matrix_mul_job fallback = { lhs, rhs, out, 0, lhs_rows };
            matrix_mul_worker_block(&fallback);
            return out;
        }
    }
    for (size_t t = 0; t < thread_count; ++t) {
        pthread_join(threads[t], NULL);
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
