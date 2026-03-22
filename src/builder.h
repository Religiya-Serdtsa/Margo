#pragma once

#include <stdbool.h>

#include "diagnostics.h"

/**
 * @file builder.h
 * @brief Declares the high-level build helpers wrapping the transpiler and the
 *        clang invocation pipeline.
 */

/**
 * @brief Transpile a `.margo` file and ask clang to emit LLVM IR.
 */
bool margo_emit_llvm_ir(const char *input_path, const char *output_path, const char *clang_path, diagnostic_t *diag);

/**
 * @brief Transpile a `.margo` file and request a native binary from clang.
 */
bool margo_build_binary(const char *input_path, const char *output_path, const char *clang_path, diagnostic_t *diag);
