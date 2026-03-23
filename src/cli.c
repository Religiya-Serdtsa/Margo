#include "cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builder.h"
#include "diagnostics.h"

typedef struct {
    const char *input;
    const char *output;
    const char *clang_bin;
    const char *llvm_output;
    bool emit_llvm;
} build_opts_t;

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s build <source.margo> -o <binary> [--clang CLANG] [--emit-llvm <path>]\n", prog);
}

static bool parse_build_args(int argc, char **argv, build_opts_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->output = "a.out";
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            opts->output = argv[++i];
            continue;
        }
        if (strcmp(arg, "--cc") == 0 || strcmp(arg, "--clang") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            opts->clang_bin = argv[++i];
            continue;
        }
        if (strcmp(arg, "--emit-llvm") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            opts->llvm_output = argv[++i];
            opts->emit_llvm = true;
            continue;
        }
        opts->input = arg;
    }
    if (!opts->input) {
        return false;
    }
    return true;
}

int margo_cli_run(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[1], "build") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    build_opts_t opts;
    if (!parse_build_args(argc - 2, &argv[2], &opts)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    diagnostic_t diag;
    if (opts.emit_llvm) {
        if (!margo_emit_llvm_ir(opts.input, opts.llvm_output, opts.clang_bin, &diag)) {
            fprintf(stderr, "LLVM emission error: %s", diag.message);
            if (diag.line) {
                fprintf(stderr, " (line %zu)", diag.line);
            }
            fprintf(stderr, "\n");
            return EXIT_FAILURE;
        }
    }
    if (!margo_build_binary(opts.input, opts.output, opts.clang_bin, &diag)) {
        fprintf(stderr, "Build error: %s", diag.message);
        if (diag.line) {
            fprintf(stderr, " (line %zu)", diag.line);
        }
        fprintf(stderr, "\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
