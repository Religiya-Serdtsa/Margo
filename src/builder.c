#define _GNU_SOURCE
#include "builder.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "transpiler.h"
#include "libttak_bundle.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
extern char **environ;

/**
 * @file builder.c
 * @brief Implements the glue between the transpiler and clang. Although the
 *        file orchestrates only a handful of steps, we annotate each helper
 *        exhaustively so future maintainers understand why the seemingly simple
 *        logic exists.
 */

/**
 * @brief Holds paths to the temporary libttak bundle extracted for each build.
 *
 * We store the root folder, the include directory, and the static library path
 * because clang consumes each of them separately. The array sizes reflect
 * PATH_MAX to avoid dynamic allocations in this critical hot path.
 */
typedef struct {
    char root[PATH_MAX];
    char include_dir[PATH_MAX];
    char lib_path[PATH_MAX];
} libttak_bundle_paths_t;

/**
 * @brief Compose `base` + `suffix` into the destination buffer.
 *
 * The helper avoids `snprintf` because concatenation is cheaper and we already
 * know the two components. It exists solely to keep the bundle-preparation code
 * readable despite the heavy error handling.
 */
static bool join_path(char *dest, size_t dest_sz, const char *base, const char *suffix, diagnostic_t *diag) {
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    if (base_len + suffix_len + 1 > dest_sz) {
        diagnostic_set(diag, 0, "path too long when composing %s%s", base, suffix);
        return false;
    }
    memcpy(dest, base, base_len);
    memcpy(dest + base_len, suffix, suffix_len + 1);
    return true;
}

/**
 * @brief Spawn a short-lived subprocess and wait for completion.
 *
 * We use this for shell utilities such as `tar` or `rm -rf`. The wrapper keeps
 * diagnostic reporting consistent while hiding the slightly clunky
 * `posix_spawnp` boilerplate.
 */
static bool run_simple_command(char *const argv[], const char *desc, diagnostic_t *diag) {
    pid_t pid;
    int spawn_rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (spawn_rc != 0) {
        if (diag && desc) {
            diagnostic_set(diag, 0, "failed to launch %s: %s", desc, strerror(spawn_rc));
        }
        return false;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        if (diag && desc) {
            diagnostic_set(diag, 0, "waitpid failed for %s: %s", desc, strerror(errno));
        }
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (diag && desc) {
            diagnostic_set(diag, 0, "%s exited with status %d", desc, WEXITSTATUS(status));
        }
        return false;
    }
    return true;
}

/**
 * @brief Dump the embedded libttak tarball to disk.
 *
 * The bundle is generated at build time via `xxd`, and this helper simply copies
 * the bytes into a temporary path so we can extract them with `tar`.
 */
static bool write_bundle_file(const char *path, diagnostic_t *diag) {
    FILE *out = fopen(path, "wb");
    if (!out) {
        diagnostic_set(diag, 0, "failed to open %s: %s", path, strerror(errno));
        return false;
    }
    size_t total = (size_t)libttak_bundle_tar_len;
    size_t written = fwrite(libttak_bundle_tar, 1, total, out);
    if (written != total) {
        diagnostic_set(diag, 0, "failed to write libttak bundle: %s", strerror(errno));
        fclose(out);
        return false;
    }
    if (fclose(out) != 0) {
        diagnostic_set(diag, 0, "failed to flush %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

/**
 * @brief Delete the temporary libttak directory if it exists.
 */
static void cleanup_libttak_bundle(libttak_bundle_paths_t *bundle) {
    if (!bundle->root[0]) {
        return;
    }
    char *argv[] = {(char *)"rm", (char *)"-rf", bundle->root, NULL};
    run_simple_command(argv, NULL, NULL);
    bundle->root[0] = '\0';
}

/**
 * @brief Extract the embedded libttak bundle into a unique directory.
 *
 * Clang needs headers and the static library during each build. Rather than
 * shipping them separately, we create a temp directory, dump the tarball, and
 * extract it. The verbose diagnostics in this function make debugging build
 * issues significantly easier.
 */
static bool prepare_libttak_bundle(libttak_bundle_paths_t *bundle, diagnostic_t *diag) {
    memset(bundle, 0, sizeof(*bundle));
    char temp_template[] = "/tmp/margo-lt-XXXXXX";
    char *dir = mkdtemp(temp_template);
    if (!dir) {
        diagnostic_set(diag, 0, "mkdtemp failed: %s", strerror(errno));
        return false;
    }
    snprintf(bundle->root, sizeof(bundle->root), "%s", dir);

    char tar_path[PATH_MAX];
    if (!join_path(tar_path, sizeof(tar_path), bundle->root, "/libttak_bundle.tar", diag)) {
        cleanup_libttak_bundle(bundle);
        return false;
    }
    if (!write_bundle_file(tar_path, diag)) {
        cleanup_libttak_bundle(bundle);
        return false;
    }

    char *tar_argv[] = {(char *)"tar", (char *)"-xf", tar_path, (char *)"-C", bundle->root, NULL};
    if (!run_simple_command(tar_argv, "tar", diag)) {
        unlink(tar_path);
        cleanup_libttak_bundle(bundle);
        return false;
    }
    unlink(tar_path);

    if (!join_path(bundle->include_dir, sizeof(bundle->include_dir), bundle->root, "/include", diag) ||
        !join_path(bundle->lib_path, sizeof(bundle->lib_path), bundle->root, "/lib/libttak.a", diag)) {
        cleanup_libttak_bundle(bundle);
        return false;
    }

    struct stat st;
    if (stat(bundle->include_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        diagnostic_set(diag, 0, "libttak bundle missing include directory");
        cleanup_libttak_bundle(bundle);
        return false;
    }
    if (stat(bundle->lib_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        diagnostic_set(diag, 0, "libttak bundle missing library payload");
        cleanup_libttak_bundle(bundle);
        return false;
    }
    return true;
}

/**
 * @brief Decide which clang binary to call, honoring CLI flags and env vars.
 */
static const char *resolve_clang(const char *clang_path) {
    const char *env = getenv("CLANG");
    if (clang_path && *clang_path) {
        return clang_path;
    }
    if (env && *env) {
        return env;
    }
    return "clang";
}

/**
 * @brief Invoke clang, streaming the transpiled source via stdin.
 *
 * The transpiler keeps everything in-memory, so this helper wires a pipe that
 * feeds clang directly. We optionally toggle `-S -emit-llvm` when LLVM IR is
 * requested. Because the function is lengthy, we keep an equally detailed
 * comment block to document every subtle resource-management step.
 */
static bool spawn_clang_with_source(const char *clang_path,
                                    const char *source,
                                    size_t source_len,
                                    const char *output_path,
                                    bool emit_llvm,
                                    diagnostic_t *diag) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) == -1) {
        diagnostic_set(diag, 0, "pipe failed: %s", strerror(errno));
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    libttak_bundle_paths_t bundle;
    bool bundle_ready = prepare_libttak_bundle(&bundle, diag);
    if (!bundle_ready) {
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    char include_flag[PATH_MAX + 3];
    snprintf(include_flag, sizeof(include_flag), "-I%s", bundle.include_dir);

    const char *argv_storage[32];
    size_t arg_index = 0;
    argv_storage[arg_index++] = clang_path;
    argv_storage[arg_index++] = "-std=c17";
    argv_storage[arg_index++] = "-D_POSIX_C_SOURCE=200809L";
    if (emit_llvm) {
        argv_storage[arg_index++] = "-S";
        argv_storage[arg_index++] = "-emit-llvm";
    } else {
        argv_storage[arg_index++] = "-O2";
    }
    argv_storage[arg_index++] = include_flag;
    argv_storage[arg_index++] = "-x";
    argv_storage[arg_index++] = "c";
    argv_storage[arg_index++] = "-";
    argv_storage[arg_index++] = "-o";
    argv_storage[arg_index++] = output_path;
    if (!emit_llvm) {
        argv_storage[arg_index++] = "-x";
        argv_storage[arg_index++] = "none";
        argv_storage[arg_index++] = bundle.lib_path;
        argv_storage[arg_index++] = "-lpthread";
        argv_storage[arg_index++] = "-lm";
        argv_storage[arg_index++] = "-lrt";
    }
    argv_storage[arg_index] = NULL;

    pid_t pid;
    int spawn_result = posix_spawnp(&pid, clang_path, &actions, NULL, (char *const *)argv_storage, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_result != 0) {
        diagnostic_set(diag, 0, "failed to launch %s: %s", clang_path, strerror(spawn_result));
        close(pipefd[0]);
        close(pipefd[1]);
        cleanup_libttak_bundle(&bundle);
        return false;
    }

    close(pipefd[0]);
    size_t written = 0;
    while (written < source_len) {
        ssize_t chunk = write(pipefd[1], source + written, source_len - written);
        if (chunk == -1) {
            if (errno == EINTR) {
                continue;
            }
            diagnostic_set(diag, 0, "failed to stream source into %s: %s", clang_path, strerror(errno));
            close(pipefd[1]);
            waitpid(pid, NULL, 0);
            cleanup_libttak_bundle(&bundle);
            return false;
        }
        written += (size_t)chunk;
    }
    close(pipefd[1]);

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        diagnostic_set(diag, 0, "waitpid failed: %s", strerror(errno));
        cleanup_libttak_bundle(&bundle);
        return false;
    }
    if (!WIFEXITED(status)) {
        diagnostic_set(diag, 0, "%s terminated abnormally", clang_path);
        cleanup_libttak_bundle(&bundle);
        return false;
    }
    if (WEXITSTATUS(status) != 0) {
        diagnostic_set(diag, 0, "%s failed with status %d", clang_path, WEXITSTATUS(status));
        cleanup_libttak_bundle(&bundle);
        return false;
    }
    cleanup_libttak_bundle(&bundle);
    return true;
}

/**
 * @brief High-level helper used by the CLI to emit LLVM IR.
 *
 * All heavy lifting lives in `margo_transpile_to_buffer` and
 * `spawn_clang_with_source`. This wrapper simply connects the dots and exposes
 * a clean API to `main.c`.
 */
bool margo_emit_llvm_ir(const char *input_path, const char *output_path, const char *clang_path, diagnostic_t *diag) {
    diagnostic_clear(diag);
    char *buffer = NULL;
    size_t size = 0;
    if (!margo_transpile_to_buffer(input_path, &buffer, &size, diag)) {
        return false;
    }
    const char *clang_bin = resolve_clang(clang_path);
    bool ok = spawn_clang_with_source(clang_bin, buffer, size, output_path, true, diag);
    free(buffer);
    return ok;
}

/**
 * @brief Equivalent to `margo_emit_llvm_ir` but requests a native binary.
 */
bool margo_build_binary(const char *input_path, const char *output_path, const char *clang_path, diagnostic_t *diag) {
    diagnostic_clear(diag);
    char *buffer = NULL;
    size_t size = 0;
    if (!margo_transpile_to_buffer(input_path, &buffer, &size, diag)) {
        return false;
    }
    const char *clang_bin = resolve_clang(clang_path);
    bool ok = spawn_clang_with_source(clang_bin, buffer, size, output_path, false, diag);
    free(buffer);
    return ok;
}
