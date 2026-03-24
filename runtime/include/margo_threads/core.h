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
 * Basic worker signature used by the lightweight scheduler.  Every job
 * receives an integer thread identifier along with the opaque user payload
 * supplied at spawn time.
 */
typedef void (*threads_job_fn)(int thread_id, void *user_data);

/** Simple bounded mailbox that writers/readers inside a cluster can share. */
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
} threads_mailbox_t;

/** Runtime stats snapshot. */
typedef struct {
    uint64_t active_threads;
    uint64_t dispatched;
    char name[32];
} threads_cluster_status_t;

/** Thread cluster definition. */
typedef struct threads_cluster {
    char name[32];
    atomic_int active_threads;
    atomic_uint_least64_t dispatch_count;
    pthread_mutex_t id_lock;
    pthread_mutex_t isolate_lock;
    int next_id;
} threads_cluster_t;

/** Handle representing a spawned job. */
typedef struct {
    pthread_t thread;
    atomic_bool finished;
    bool joined;
    int id;
} threads_job_handle_t;

/** Triple used by `_pairs` semantics. */
typedef struct {
    threads_job_handle_t primary;
    threads_job_handle_t shared_a;
    threads_job_handle_t shared_b;
} threads_pair_group_t;

typedef struct {
    threads_job_fn primary;
    threads_job_fn shared_a;
    threads_job_fn shared_b;
    void *payload;
} threads_pair_spec_t;

typedef struct {
    void *ptr;
    size_t size;
} threads_isolated_region_t;

static inline uint64_t threads_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline double threads_ns_to_ms(uint64_t ns) {
    return (double)ns / 1000000.0;
}

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
    pthread_mutex_init(&cluster->isolate_lock, NULL);
    cluster->next_id = 0;
}

static inline void threads_cluster_destroy(threads_cluster_t *cluster) {
    if (!cluster) {
        return;
    }
    pthread_mutex_destroy(&cluster->id_lock);
    pthread_mutex_destroy(&cluster->isolate_lock);
}

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

static inline bool threads_mailbox_init(threads_mailbox_t *box, size_t elem_size, size_t capacity) {
    if (!box || elem_size == 0 || capacity == 0) {
        return false;
    }
    memset(box, 0, sizeof(*box));
    box->elem_size = elem_size;
    box->capacity = capacity;
    box->buffer = malloc(elem_size * capacity);
    if (!box->buffer) {
        return false;
    }
    pthread_mutex_init(&box->lock, NULL);
    pthread_cond_init(&box->not_empty, NULL);
    pthread_cond_init(&box->not_full, NULL);
    return true;
}

static inline void threads_mailbox_destroy(threads_mailbox_t *box) {
    if (!box) {
        return;
    }
    pthread_mutex_destroy(&box->lock);
    pthread_cond_destroy(&box->not_empty);
    pthread_cond_destroy(&box->not_full);
    free(box->buffer);
    box->buffer = NULL;
    box->capacity = box->elem_size = 0;
    box->count = box->head = box->tail = 0;
    box->closed = true;
}

static inline bool threads_mailbox_push_ex(threads_mailbox_t *box,
                                           const void *item,
                                           bool blocking) {
    if (!box || !item) {
        return false;
    }
    pthread_mutex_lock(&box->lock);
    while (!box->closed && box->count == box->capacity) {
        if (!blocking) {
            pthread_mutex_unlock(&box->lock);
            return false;
        }
        pthread_cond_wait(&box->not_full, &box->lock);
    }
    if (box->closed) {
        pthread_mutex_unlock(&box->lock);
        return false;
    }
    memcpy(box->buffer + (box->tail * box->elem_size), item, box->elem_size);
    box->tail = (box->tail + 1) % box->capacity;
    box->count++;
    pthread_cond_signal(&box->not_empty);
    pthread_mutex_unlock(&box->lock);
    return true;
}

static inline bool threads_mailbox_pop_ex(threads_mailbox_t *box, void *out, bool blocking) {
    if (!box || !out) {
        return false;
    }
    pthread_mutex_lock(&box->lock);
    while (box->count == 0 && !box->closed) {
        if (!blocking) {
            pthread_mutex_unlock(&box->lock);
            return false;
        }
        pthread_cond_wait(&box->not_empty, &box->lock);
    }
    if (box->count == 0 && box->closed) {
        pthread_mutex_unlock(&box->lock);
        return false;
    }
    memcpy(out, box->buffer + (box->head * box->elem_size), box->elem_size);
    box->head = (box->head + 1) % box->capacity;
    box->count--;
    pthread_cond_signal(&box->not_full);
    pthread_mutex_unlock(&box->lock);
    return true;
}

static inline bool threads_mailbox_send(threads_mailbox_t *box, const void *item) {
    return threads_mailbox_push_ex(box, item, true);
}

static inline bool threads_mailbox_try_send(threads_mailbox_t *box, const void *item) {
    return threads_mailbox_push_ex(box, item, false);
}

static inline bool threads_mailbox_recv(threads_mailbox_t *box, void *out) {
    return threads_mailbox_pop_ex(box, out, true);
}

static inline bool threads_mailbox_try_recv(threads_mailbox_t *box, void *out) {
    return threads_mailbox_pop_ex(box, out, false);
}

static inline void threads_mailbox_close(threads_mailbox_t *box) {
    if (!box) {
        return;
    }
    pthread_mutex_lock(&box->lock);
    box->closed = true;
    pthread_cond_broadcast(&box->not_empty);
    pthread_cond_broadcast(&box->not_full);
    pthread_mutex_unlock(&box->lock);
}

static inline size_t threads_mailbox_size(threads_mailbox_t *box) {
    if (!box) {
        return 0;
    }
    pthread_mutex_lock(&box->lock);
    size_t count = box->count;
    pthread_mutex_unlock(&box->lock);
    return count;
}

typedef struct {
    threads_cluster_t *cluster;
    threads_job_fn fn;
    void *payload;
    bool isolate;
    int id;
    threads_job_handle_t *handle;
} threads_job_ctx_t;

static inline int threads_cluster_next_id(threads_cluster_t *cluster) {
    pthread_mutex_lock(&cluster->id_lock);
    int id = cluster->next_id++;
    pthread_mutex_unlock(&cluster->id_lock);
    return id;
}

static inline void *threads_job_trampoline(void *arg) {
    threads_job_ctx_t *ctx = (threads_job_ctx_t *)arg;
    if (ctx->isolate) {
        pthread_mutex_lock(&ctx->cluster->isolate_lock);
    }
    atomic_fetch_add(&ctx->cluster->active_threads, 1);
    atomic_fetch_add(&ctx->cluster->dispatch_count, 1);
    ctx->fn(ctx->id, ctx->payload);
    atomic_fetch_sub(&ctx->cluster->active_threads, 1);
    atomic_store(&ctx->handle->finished, true);
    if (ctx->isolate) {
        pthread_mutex_unlock(&ctx->cluster->isolate_lock);
    }
    free(ctx);
    return NULL;
}

static inline bool threads_spawn_internal(threads_cluster_t *cluster,
                                          threads_job_fn fn,
                                          void *payload,
                                          bool isolate,
                                          threads_job_handle_t *out_handle) {
    if (!cluster || !fn || !out_handle) {
        return false;
    }
    threads_job_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->cluster = cluster;
    ctx->fn = fn;
    ctx->payload = payload;
    ctx->isolate = isolate;
    ctx->id = threads_cluster_next_id(cluster);
    ctx->handle = out_handle;
    memset(out_handle, 0, sizeof(*out_handle));
    out_handle->id = ctx->id;
    atomic_init(&out_handle->finished, false);
    int rc = pthread_create(&out_handle->thread, NULL, threads_job_trampoline, ctx);
    if (rc != 0) {
        free(ctx);
        memset(out_handle, 0, sizeof(*out_handle));
        return false;
    }
    out_handle->joined = false;
    return true;
}

static inline bool threads_spawn(threads_cluster_t *cluster,
                                 threads_job_fn fn,
                                 void *payload,
                                 threads_job_handle_t *out_handle) {
    return threads_spawn_internal(cluster, fn, payload, false, out_handle);
}

static inline bool threads_spawn_isolate(threads_cluster_t *cluster,
                                         threads_job_fn fn,
                                         void *payload,
                                         threads_job_handle_t *out_handle) {
    return threads_spawn_internal(cluster, fn, payload, true, out_handle);
}

static inline bool threads_job_join(threads_job_handle_t *handle) {
    if (!handle || handle->joined || handle->thread == (pthread_t)0) {
        return true;
    }
    if (pthread_join(handle->thread, NULL) == 0) {
        handle->joined = true;
        atomic_store(&handle->finished, true);
        return true;
    }
    return false;
}

static inline bool threads_job_wait_ms(threads_job_handle_t *handle, int timeout_ms) {
    if (!handle) {
        return false;
    }
    if (timeout_ms < 0) {
        return threads_job_join(handle);
    }
    const int sleep_ms = 1;
    while (timeout_ms > 0) {
        if (atomic_load(&handle->finished)) {
            return threads_job_join(handle);
        }
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
        timeout_ms -= sleep_ms;
    }
    return atomic_load(&handle->finished);
}

static inline void threads_job_detach(threads_job_handle_t *handle) {
    if (!handle || handle->joined || handle->thread == (pthread_t)0) {
        return;
    }
    pthread_detach(handle->thread);
    handle->joined = true;
}

static inline bool threads_job_group_join(threads_job_handle_t *handles, size_t count) {
    bool ok = true;
    if (!handles) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!threads_job_join(&handles[i])) {
            ok = false;
        }
    }
    return ok;
}

static inline bool threads_spawn_pairs(threads_cluster_t *cluster,
                                       const threads_pair_spec_t *spec,
                                       threads_pair_group_t *group) {
    if (!cluster || !spec || !group) {
        return false;
    }
    memset(group, 0, sizeof(*group));
    bool ok = true;
    if (spec->primary) {
        ok &= threads_spawn(cluster, spec->primary, spec->payload, &group->primary);
    }
    if (spec->shared_a) {
        ok &= threads_spawn(cluster, spec->shared_a, spec->payload, &group->shared_a);
    }
    if (spec->shared_b) {
        ok &= threads_spawn(cluster, spec->shared_b, spec->payload, &group->shared_b);
    }
    return ok;
}

static inline void threads_pairs_join(threads_pair_group_t *group) {
    if (!group) {
        return;
    }
    threads_job_handle_t handles[3] = {group->primary, group->shared_a, group->shared_b};
    threads_job_group_join(handles, 3);
    group->primary = handles[0];
    group->shared_a = handles[1];
    group->shared_b = handles[2];
}

static inline bool threads_isolated_region_alloc(threads_isolated_region_t *region, size_t size) {
    if (!region || size == 0) {
        return false;
    }
    void *ptr = NULL;
    if (posix_memalign(&ptr, 64, size) != 0) {
        return false;
    }
    memset(ptr, 0, size);
    region->ptr = ptr;
    region->size = size;
    return true;
}

static inline void threads_isolated_region_free(threads_isolated_region_t *region) {
    if (!region || !region->ptr) {
        return;
    }
    free(region->ptr);
    region->ptr = NULL;
    region->size = 0;
}

#ifdef __cplusplus
}
#endif
