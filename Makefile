CC ?= gcc
CFLAGS ?= -std=c17 -Wall -Wextra -pedantic -Isrc
LDFLAGS ?=

LIBTTAK_DIR := deps/libttak
LIBTTAK_LIB := $(abspath $(LIBTTAK_DIR)/lib/libttak.a)
LIBTTAK_BUNDLE := build/libttak_bundle.tar
LIBTTAK_EMBED_SRC := src/libttak_bundle.c
LIBTTAK_EMBED_HDR := src/libttak_bundle.h
RUNTIME_DIR := runtime
RUNTIME_ABS := $(abspath $(RUNTIME_DIR))
RUNTIME_INCLUDE_DIR := $(RUNTIME_DIR)/include

MARGO_SRCS = \
	src/main.c \
	src/cli.c \
	src/diagnostics.c \
	src/lexer.c \
	src/parser.c \
	src/sema.c \
	src/transpiler.c \
	src/builder.c \
	$(LIBTTAK_EMBED_SRC)

MARGO_OBJS = $(MARGO_SRCS:.c=.o)

all: build/margo

build/margo-c: $(MARGO_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(MARGO_OBJS) $(LDFLAGS)

build/margo: margo-in-margo
	@mkdir -p $(@D)
	cp margo_build/margo $@

$(LIBTTAK_LIB):
	$(MAKE) -C $(LIBTTAK_DIR) EMBEDDED=1

$(LIBTTAK_BUNDLE): $(LIBTTAK_LIB) $(RUNTIME_INCLUDE_DIR)/margo_threads/core.h $(RUNTIME_INCLUDE_DIR)/margo_process/core.h $(RUNTIME_INCLUDE_DIR)/margo_matrix/core.h $(RUNTIME_INCLUDE_DIR)/margo_std/slice.h $(RUNTIME_INCLUDE_DIR)/margo_std/owned.h
	@mkdir -p $(@D)
	tar -cf $@ -C $(LIBTTAK_DIR) include lib/libttak.a -C $(RUNTIME_ABS) include/margo_threads include/margo_process include/margo_matrix include/margo_std

$(LIBTTAK_EMBED_SRC): $(LIBTTAK_BUNDLE)
	@echo "Embedding libttak bundle"
	@xxd -i $< | sed 's/unsigned /const unsigned /g' | sed 's/build_libttak_bundle_tar/libttak_bundle_tar/g' > $@

$(LIBTTAK_EMBED_HDR): $(LIBTTAK_EMBED_SRC)
	@printf "#pragma once\nextern const unsigned char libttak_bundle_tar[];\nextern const unsigned int libttak_bundle_tar_len;\n" > $@

src/main.o: src/main.c
src/diagnostics.o: src/diagnostics.c
src/lexer.o: src/lexer.c
src/parser.o: src/parser.c
src/sema.o: src/sema.c src/sema.h src/lexer.h src/diagnostics.h
src/transpiler.o: src/transpiler.c
src/builder.o: src/builder.c $(LIBTTAK_EMBED_HDR)
src/libttak_bundle.o: $(LIBTTAK_EMBED_SRC) $(LIBTTAK_EMBED_HDR)

clean:
	rm -f $(MARGO_OBJS)
	rm -rf build
	# margo-in-margo self-compilation artifacts
	rm -rf margo_build
	$(MAKE) -C $(LIBTTAK_DIR) clean
	rm -f $(LIBTTAK_EMBED_SRC) $(LIBTTAK_EMBED_HDR)

margo-in-margo: build/margo-c
	@mkdir -p margo_build
	./build/margo-c build experiment/margo_compiler.margo -o margo_build/margo_stage1
	./margo_build/margo_stage1 build experiment/margo_compiler.margo -o margo_build/margo
	cp ./margo_build/margo ./margo_build/margo_stage2
	./margo_build/margo_stage2 build experiment/margo_compiler.margo -o margo_build/margo
	@echo "self-compilation complete: margo_build/margo"

.PHONY: all clean margo-in-margo
