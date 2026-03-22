static int margo_validate_cli(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s build <source.margo> -o <binary>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "build") != 0) {
        fprintf(stderr, "margo-in-margo: only 'build' is supported, got: %s\n", argv[1]);
        return 1;
    }
    int has_output_flag = 0;
    for (int i = 3; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && argv[i + 1][0] != '\0') {
            has_output_flag = 1;
            break;
        }
    }
    if (!has_output_flag) {
        fprintf(stderr, "margo-in-margo: missing required '-o <binary>' argument\n");
        return 1;
    }
    return 0;
}
