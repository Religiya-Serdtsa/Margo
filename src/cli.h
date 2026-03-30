#pragma once

/**
 * @file cli.h
 * @brief Command-line entry point for the Margo compiler frontend.
 */

/**
 * @brief Execute the Margo CLI with argv-style arguments.
 *
 * @param argc Number of arguments.
 * @param argv Argument vector.
 * @return Process exit code (0 on success).
 */
int margo_cli_run(int argc, char **argv);
