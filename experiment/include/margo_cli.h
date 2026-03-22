static int margo_validate_cli(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s build <source.margo> -o <binary>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "build") != 0) {
        fprintf(stderr, "margo-in-margo: only 'build' is supported, got: %s\n", argv[1]);
        return 1;
    }
    return 0;
}
