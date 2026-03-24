#pragma once

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct process_channel_shared {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    size_t elem_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int closed;
    uint8_t buffer[];
} process_channel_shared_t;

typedef struct {
    process_channel_shared_t *shared;
    size_t alloc_size;
} process_channel_t;

static inline bool process_channel_init(process_channel_t *chan,
                                        size_t capacity,
                                        size_t elem_size) {
    if (!chan || capacity == 0 || elem_size == 0) {
        return false;
    }
    size_t alloc_size = sizeof(process_channel_shared_t) + (capacity * elem_size);
    process_channel_shared_t *shared = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        return false;
    }
    memset(shared, 0, alloc_size);
    shared->elem_size = elem_size;
    shared->capacity = capacity;

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shared->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&shared->not_empty, &cattr);
    pthread_cond_init(&shared->not_full, &cattr);
    pthread_condattr_destroy(&cattr);

    chan->shared = shared;
    chan->alloc_size = alloc_size;
    return true;
}

static inline void process_channel_destroy(process_channel_t *chan) {
    if (!chan || !chan->shared) {
        return;
    }
    process_channel_shared_t *shared = chan->shared;
    pthread_mutex_destroy(&shared->mutex);
    pthread_cond_destroy(&shared->not_empty);
    pthread_cond_destroy(&shared->not_full);
    munmap(shared, chan->alloc_size);
    chan->shared = NULL;
    chan->alloc_size = 0;
}

static inline bool process_channel_send_ex(process_channel_t *chan,
                                           const void *value,
                                           bool blocking) {
    if (!chan || !chan->shared || !value) {
        return false;
    }
    process_channel_shared_t *shared = chan->shared;
    pthread_mutex_lock(&shared->mutex);
    while (!shared->closed && shared->count == shared->capacity) {
        if (!blocking) {
            pthread_mutex_unlock(&shared->mutex);
            return false;
        }
        pthread_cond_wait(&shared->not_full, &shared->mutex);
    }
    if (shared->closed) {
        pthread_mutex_unlock(&shared->mutex);
        return false;
    }
    memcpy(shared->buffer + (shared->tail * shared->elem_size), value, shared->elem_size);
    shared->tail = (shared->tail + 1) % shared->capacity;
    shared->count++;
    pthread_cond_signal(&shared->not_empty);
    pthread_mutex_unlock(&shared->mutex);
    return true;
}

static inline bool process_channel_recv_ex(process_channel_t *chan, void *out, bool blocking) {
    if (!chan || !chan->shared || !out) {
        return false;
    }
    process_channel_shared_t *shared = chan->shared;
    pthread_mutex_lock(&shared->mutex);
    while (shared->count == 0 && !shared->closed) {
        if (!blocking) {
            pthread_mutex_unlock(&shared->mutex);
            return false;
        }
        pthread_cond_wait(&shared->not_empty, &shared->mutex);
    }
    if (shared->count == 0 && shared->closed) {
        pthread_mutex_unlock(&shared->mutex);
        return false;
    }
    memcpy(out, shared->buffer + (shared->head * shared->elem_size), shared->elem_size);
    shared->head = (shared->head + 1) % shared->capacity;
    shared->count--;
    pthread_cond_signal(&shared->not_full);
    pthread_mutex_unlock(&shared->mutex);
    return true;
}

static inline bool process_channel_send(process_channel_t *chan, const void *value) {
    return process_channel_send_ex(chan, value, true);
}

static inline bool process_channel_try_send(process_channel_t *chan, const void *value) {
    return process_channel_send_ex(chan, value, false);
}

static inline bool process_channel_recv(process_channel_t *chan, void *out) {
    return process_channel_recv_ex(chan, out, true);
}

static inline bool process_channel_try_recv(process_channel_t *chan, void *out) {
    return process_channel_recv_ex(chan, out, false);
}

static inline void process_channel_close(process_channel_t *chan) {
    if (!chan || !chan->shared) {
        return;
    }
    process_channel_shared_t *shared = chan->shared;
    pthread_mutex_lock(&shared->mutex);
    shared->closed = 1;
    pthread_cond_broadcast(&shared->not_empty);
    pthread_cond_broadcast(&shared->not_full);
    pthread_mutex_unlock(&shared->mutex);
}

static inline size_t process_channel_size(process_channel_t *chan) {
    if (!chan || !chan->shared) {
        return 0;
    }
    process_channel_shared_t *shared = chan->shared;
    pthread_mutex_lock(&shared->mutex);
    size_t size = shared->count;
    pthread_mutex_unlock(&shared->mutex);
    return size;
}

typedef struct {
    void *addr;
    size_t size;
} process_shared_region_t;

static inline bool process_shared_region_alloc(process_shared_region_t *region, size_t size) {
    if (!region || size == 0) {
        return false;
    }
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return false;
    }
    memset(addr, 0, size);
    region->addr = addr;
    region->size = size;
    return true;
}

static inline void process_shared_region_free(process_shared_region_t *region) {
    if (!region || !region->addr) {
        return;
    }
    munmap(region->addr, region->size);
    region->addr = NULL;
    region->size = 0;
}

static inline bool process_map_file(process_shared_region_t *region,
                                    const char *path,
                                    size_t size,
                                    int prot,
                                    int flags) {
    if (!region || !path) {
        return false;
    }
    int fd = open(path, (flags & O_WRONLY) ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        return false;
    }
    size_t length = size ? size : (size_t)st.st_size;
    void *addr = mmap(NULL, length, prot, MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        return false;
    }
    region->addr = addr;
    region->size = length;
    return true;
}

typedef void (*process_entry_fn)(process_channel_t *chan, void *user_state);

typedef struct {
    pid_t pid;
    process_channel_t *chan;
    void *user_state;
} process_handle_t;

#ifndef PROCESS_CLUSTER_MAX
#define PROCESS_CLUSTER_MAX 32
#endif

typedef struct {
    char name[32];
    size_t count;
    process_handle_t handles[PROCESS_CLUSTER_MAX];
} process_cluster_t;

static inline void process_cluster_init(process_cluster_t *cluster, const char *name) {
    if (!cluster) {
        return;
    }
    memset(cluster, 0, sizeof(*cluster));
    if (name && *name) {
        strncpy(cluster->name, name, sizeof(cluster->name) - 1);
    } else {
        strncpy(cluster->name, "process", sizeof(cluster->name) - 1);
    }
}

static inline bool process_cluster_spawn(process_cluster_t *cluster,
                                         process_entry_fn entry,
                                         process_channel_t *chan,
                                         void *user_state) {
    if (!cluster || !entry || cluster->count >= PROCESS_CLUSTER_MAX) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        entry(chan, user_state);
        _exit(EXIT_SUCCESS);
    }
    process_handle_t *handle = &cluster->handles[cluster->count++];
    handle->pid = pid;
    handle->chan = chan;
    handle->user_state = user_state;
    return true;
}

static inline bool process_cluster_wait_all(process_cluster_t *cluster) {
    if (!cluster) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < cluster->count; ++i) {
        int status = 0;
        if (waitpid(cluster->handles[i].pid, &status, 0) == -1) {
            ok = false;
            continue;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            ok = false;
        }
    }
    cluster->count = 0;
    return ok;
}

typedef void (*process_supervisor_fn)(pid_t pid, int status, void *user_data);

static inline void process_cluster_supervise(process_cluster_t *cluster,
                                             process_supervisor_fn fn,
                                             void *user_data) {
    if (!cluster || !fn) {
        return;
    }
    for (size_t i = 0; i < cluster->count; ++i) {
        int status = 0;
        waitpid(cluster->handles[i].pid, &status, 0);
        fn(cluster->handles[i].pid, status, user_data);
    }
    cluster->count = 0;
}

#ifdef __cplusplus
}
#endif
