#pragma once

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file core.h
 * @brief Thread runtime helpers backing `@import threads/core`.
 */

/** @defgroup threads_runtime Threads Runtime */
/** @{ */

/**
 * Simple worker signature used by the thread cluster runtime. Each thread
 * receives an integer identifier plus the opaque payload supplied at init.
 */
typedef void (*threads_thread_fn)(int thread_id, void *user_data);

/**
 * Blocking channel used by cluster threads. Acts as the backing store for
 * `chan T` in the surface language while keeping a minimal C ABI.
 */
typedef struct {
    size_t elem_size;
    size_t capacity;
    uint8_t *buffer;
    size_t head;
    size_t tail;
    size_t count;
    bool closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} threads_channel_t;

/** Snapshot of runtime statistics for introspection. */
typedef struct {
    uint64_t active_threads;
    uint64_t dispatched;
    char name[32];
} threads_cluster_status_t;

/** Thread cluster state shared by all `threadN` definitions. */
typedef struct threads_cluster {
    char name[32];
    atomic_int active_threads;
    atomic_uint_least64_t dispatch_count;
    pthread_mutex_t id_lock;
    int next_id;
} threads_cluster_t;

typedef enum {
    THREADS_THREAD_FLAG_NONE = 0u,
    THREADS_THREAD_FLAG_INDEPENDENT = 1u << 0,
    THREADS_THREAD_FLAG_LAZY_JOIN = 1u << 1,
} threads_thread_flags_t;

/** Handle representing a thread declared inside a cluster. */
typedef struct threads_thread_handle {
    pthread_t thread;
    atomic_bool finished;
    bool joined;
    int id;
    threads_cluster_t *cluster;
    threads_thread_fn fn;
    void *payload;
    void *chan_binding;
    unsigned int flags;
} threads_thread_handle_t;

/** RAII helper mirroring `locked_var` semantics. */
typedef struct {
    pthread_mutex_t mutex;
} threads_locked_var_t;

/** @brief Initialize `locked_var` state. */
static inline void threads_locked_init(threads_locked_var_t *locked) {
    if (!locked) {
        return;
    }
    pthread_mutex_init(&locked->mutex, NULL);
}

/** @brief Destroy `locked_var` state. */
static inline void threads_locked_destroy(threads_locked_var_t *locked) {
    if (!locked) {
        return;
    }
    pthread_mutex_destroy(&locked->mutex);
}

/** @brief Acquire `locked_var` mutex. */
static inline void threads_locked_acquire(threads_locked_var_t *locked) {
    if (!locked) {
        return;
    }
    pthread_mutex_lock(&locked->mutex);
}

/** @brief Release `locked_var` mutex. */
static inline void threads_locked_release(threads_locked_var_t *locked) {
    if (!locked) {
        return;
    }
    pthread_mutex_unlock(&locked->mutex);
}

/** @brief Return monotonic timestamp in nanoseconds. */
static inline uint64_t threads_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/** @brief Convert nanoseconds to milliseconds. */
static inline double threads_ns_to_ms(uint64_t ns) {
    return (double)ns / 1000000.0;
}

/** @brief Initialize a thread cluster. */
static inline void threads_cluster_init(threads_cluster_t *cluster, const char *name) {
    if (!cluster) {
        return;
    }
    memset(cluster, 0, sizeof(*cluster));
    if (name && *name) {
        strncpy(cluster->name, name, sizeof(cluster->name) - 1);
    } else {
        strncpy(cluster->name, "threads", sizeof(cluster->name) - 1);
    }
    atomic_init(&cluster->active_threads, 0);
    atomic_init(&cluster->dispatch_count, 0);
    pthread_mutex_init(&cluster->id_lock, NULL);
    cluster->next_id = 0;
}

/** @brief Destroy a thread cluster. */
static inline void threads_cluster_destroy(threads_cluster_t *cluster) {
    if (!cluster) {
        return;
    }
    pthread_mutex_destroy(&cluster->id_lock);
}

/** @brief Snapshot cluster status counters. */
static inline threads_cluster_status_t threads_cluster_status(const threads_cluster_t *cluster) {
    threads_cluster_status_t status = {0};
    if (!cluster) {
        return status;
    }
    status.active_threads = (uint64_t)atomic_load(&((threads_cluster_t *)cluster)->active_threads);
    status.dispatched = (uint64_t)atomic_load(&((threads_cluster_t *)cluster)->dispatch_count);
    if (*cluster->name) {
        strncpy(status.name, cluster->name, sizeof(status.name) - 1);
    }
    return status;
}

/** @brief Initialize a blocking channel for thread communication. */
static inline bool threads_channel_init(threads_channel_t *chan, size_t elem_size, size_t capacity) {
    if (!chan || elem_size == 0 || capacity == 0) {
        errno = EINVAL;
        return false;
    }
    memset(chan, 0, sizeof(*chan));
    chan->elem_size = elem_size;
    chan->capacity = capacity;
    chan->buffer = malloc(elem_size * capacity);
    if (!chan->buffer) {
        errno = ENOMEM;
        return false;
    }
    pthread_mutex_init(&chan->lock, NULL);
    pthread_cond_init(&chan->not_empty, NULL);
    pthread_cond_init(&chan->not_full, NULL);
    return true;
}

/** @brief Destroy a thread channel and release resources. */
static inline void threads_channel_destroy(threads_channel_t *chan) {
    if (!chan) {
        return;
    }
    pthread_mutex_destroy(&chan->lock);
    pthread_cond_destroy(&chan->not_empty);
    pthread_cond_destroy(&chan->not_full);
    free(chan->buffer);
    chan->buffer = NULL;
    chan->capacity = chan->elem_size = 0;
    chan->count = chan->head = chan->tail = 0;
    chan->closed = true;
}

/** @brief Push one value to channel (blocking or non-blocking). */
static inline bool threads_channel_push_ex(threads_channel_t *chan,
                                           const void *item,
                                           bool blocking) {
    if (!chan || !item) {
        errno = EINVAL;
        return false;
    }
    pthread_mutex_lock(&chan->lock);
    while (!chan->closed && chan->count == chan->capacity) {
        if (!blocking) {
            pthread_mutex_unlock(&chan->lock);
            return false;
        }
        pthread_cond_wait(&chan->not_full, &chan->lock);
    }
    if (chan->closed) {
        pthread_mutex_unlock(&chan->lock);
        return false;
    }
    memcpy(chan->buffer + (chan->tail * chan->elem_size), item, chan->elem_size);
    chan->tail = (chan->tail + 1) % chan->capacity;
    chan->count++;
    pthread_cond_signal(&chan->not_empty);
    pthread_mutex_unlock(&chan->lock);
    return true;
}

/** @brief Pop one value from channel (blocking or non-blocking). */
static inline bool threads_channel_pop_ex(threads_channel_t *chan, void *out, bool blocking) {
    if (!chan || !out) {
        errno = EINVAL;
        return false;
    }
    pthread_mutex_lock(&chan->lock);
    while (chan->count == 0 && !chan->closed) {
        if (!blocking) {
            pthread_mutex_unlock(&chan->lock);
            return false;
        }
        pthread_cond_wait(&chan->not_empty, &chan->lock);
    }
    if (chan->count == 0 && chan->closed) {
        pthread_mutex_unlock(&chan->lock);
        return false;
    }
    memcpy(out, chan->buffer + (chan->head * chan->elem_size), chan->elem_size);
    chan->head = (chan->head + 1) % chan->capacity;
    chan->count--;
    pthread_cond_signal(&chan->not_full);
    pthread_mutex_unlock(&chan->lock);
    return true;
}

/** @brief Blocking send helper. */
static inline bool threads_channel_send(threads_channel_t *chan, const void *item) {
    return threads_channel_push_ex(chan, item, true);
}

/** @brief Non-blocking send helper. */
static inline bool threads_channel_try_send(threads_channel_t *chan, const void *item) {
    return threads_channel_push_ex(chan, item, false);
}

/** @brief Blocking receive helper. */
static inline bool threads_channel_recv(threads_channel_t *chan, void *out) {
    return threads_channel_pop_ex(chan, out, true);
}

/** @brief Non-blocking receive helper. */
static inline bool threads_channel_try_recv(threads_channel_t *chan, void *out) {
    return threads_channel_pop_ex(chan, out, false);
}

/** @brief Close channel and wake all waiters. */
static inline void threads_channel_close(threads_channel_t *chan) {
    if (!chan) {
        return;
    }
    pthread_mutex_lock(&chan->lock);
    chan->closed = true;
    pthread_cond_broadcast(&chan->not_empty);
    pthread_cond_broadcast(&chan->not_full);
    pthread_mutex_unlock(&chan->lock);
}

/** @brief Return queued element count. */
static inline size_t threads_channel_size(threads_channel_t *chan) {
    if (!chan) {
        return 0;
    }
    pthread_mutex_lock(&chan->lock);
    size_t count = chan->count;
    pthread_mutex_unlock(&chan->lock);
    return count;
}

typedef struct {
    threads_cluster_t *cluster;
    threads_thread_fn fn;
    void *payload;
    int id;
    threads_thread_handle_t *handle;
} threads_thread_ctx_t;

/** @brief Allocate next cluster-local thread id. */
static inline int threads_cluster_next_id(threads_cluster_t *cluster) {
    pthread_mutex_lock(&cluster->id_lock);
    int id = cluster->next_id++;
    pthread_mutex_unlock(&cluster->id_lock);
    return id;
}

/** @brief Internal trampoline used by `pthread_create`. */
static inline void *threads_thread_trampoline(void *arg) {
    threads_thread_ctx_t *ctx = (threads_thread_ctx_t *)arg;
    atomic_fetch_add(&ctx->cluster->active_threads, 1);
    atomic_fetch_add(&ctx->cluster->dispatch_count, 1);
    ctx->fn(ctx->id, ctx->payload);
    atomic_fetch_sub(&ctx->cluster->active_threads, 1);
    atomic_store(&ctx->handle->finished, true);
    free(ctx);
    return NULL;
}

/** @brief Start a runtime thread with flags and optional channel binding. */
static inline bool threads_thread_start(threads_cluster_t *cluster,
                                        threads_thread_fn fn,
                                        void *payload,
                                        void *chan_binding,
                                        unsigned int flags,
                                        threads_thread_handle_t *out_handle) {
    if (!cluster || !fn || !out_handle) {
        errno = EINVAL;
        return false;
    }
    threads_thread_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        errno = ENOMEM;
        return false;
    }
    ctx->cluster = cluster;
    ctx->fn = fn;
    ctx->payload = payload;
    ctx->handle = out_handle;
    ctx->id = threads_cluster_next_id(cluster);

    out_handle->cluster = cluster;
    out_handle->fn = fn;
    out_handle->payload = payload;
    out_handle->chan_binding = chan_binding;
    out_handle->flags = flags;
    out_handle->id = ctx->id;
    out_handle->joined = false;
    atomic_init(&out_handle->finished, false);

    int rc = pthread_create(&out_handle->thread, NULL, threads_thread_trampoline, ctx);
    if (rc != 0) {
        free(ctx);
        out_handle->thread = (pthread_t)0;
        out_handle->joined = true;
        atomic_store(&out_handle->finished, true);
        errno = rc;
        return false;
    }
    return true;
}

/** @brief Restart a previously joined thread handle. */
static inline bool threads_thread_restart(threads_thread_handle_t *handle) {
    if (!handle || !handle->cluster || !handle->fn) {
        errno = EINVAL;
        return false;
    }
    if (handle->thread != (pthread_t)0 && !handle->joined) {
        errno = EBUSY;
        return false;
    }
    return threads_thread_start(handle->cluster,
                                handle->fn,
                                handle->payload,
                                handle->chan_binding,
                                handle->flags,
                                handle);
}

/** @brief Join one runtime thread. */
static inline bool threads_thread_join(threads_thread_handle_t *handle) {
    if (!handle) {
        errno = EINVAL;
        return false;
    }
    if (handle->thread == (pthread_t)0 || handle->joined) {
        return true;
    }
    if (pthread_join(handle->thread, NULL) == 0) {
        handle->joined = true;
        handle->thread = (pthread_t)0;
        atomic_store(&handle->finished, true);
        return true;
    }
    return false;
}

/** @brief Join with millisecond timeout (`<0` means blocking join). */
static inline bool threads_thread_wait_ms(threads_thread_handle_t *handle, int timeout_ms) {
    if (!handle) {
        errno = EINVAL;
        return false;
    }
    if (timeout_ms < 0) {
        return threads_thread_join(handle);
    }
    const int sleep_ms = 1;
    while (timeout_ms > 0) {
        if (atomic_load(&handle->finished)) {
            return threads_thread_join(handle);
        }
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
        timeout_ms -= sleep_ms;
    }
    return atomic_load(&handle->finished);
}

/** @brief Detach a thread handle from join lifecycle. */
static inline void threads_thread_detach(threads_thread_handle_t *handle) {
    if (!handle || handle->joined || handle->thread == (pthread_t)0) {
        return;
    }
    pthread_detach(handle->thread);
    handle->joined = true;
    handle->thread = (pthread_t)0;
}

/** @brief Join all eligible handles in a cluster handle array. */
static inline bool threads_cluster_join_all(threads_thread_handle_t *handles,
                                            size_t count,
                                            void *chan_binding) {
    if (!handles) {
        errno = EINVAL;
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < count; ++i) {
        threads_thread_handle_t *handle = &handles[i];
        if (handle->thread == (pthread_t)0) {
            continue;
        }
        if ((handle->flags & THREADS_THREAD_FLAG_INDEPENDENT) != 0) {
            continue;
        }
        if ((handle->flags & THREADS_THREAD_FLAG_LAZY_JOIN) != 0) {
            continue;
        }
        if (chan_binding && handle->chan_binding != chan_binding) {
            continue;
        }
        if (!threads_thread_join(handle)) {
            ok = false;
        }
    }
    return ok;
}

/** @brief Join handles marked with lazy-join flag. */
static inline bool threads_cluster_sync(threads_thread_handle_t *handles, size_t count) {
    if (!handles) {
        errno = EINVAL;
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < count; ++i) {
        threads_thread_handle_t *handle = &handles[i];
        if (handle->thread == (pthread_t)0) {
            continue;
        }
        if ((handle->flags & THREADS_THREAD_FLAG_LAZY_JOIN) == 0) {
            continue;
        }
        if (!threads_thread_join(handle)) {
            ok = false;
        }
    }
    return ok;
}

/** @} */

#ifdef __cplusplus
}
#endif
