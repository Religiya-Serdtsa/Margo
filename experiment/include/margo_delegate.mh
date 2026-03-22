static int margo_delegate_build(int argc, char **argv) {
    if (argc < 1 || !argv || !argv[0]) {
        fprintf(stderr, "margo-in-margo: invalid argv for delegation\n");
        return 1;
    }
    argv[0] = "./build/margo";
    execv("./build/margo", argv);
    fprintf(stderr, "margo-in-margo: failed to exec ./build/margo: %s\n", strerror(errno));
    return 1;
}

static int margo_compiler_start(int argc, char **argv) {
    int validation_rc = margo_validate_cli(argc, argv);
    if (validation_rc != 0) {
        return validation_rc;
    }
    return margo_delegate_build(argc, argv);
}
