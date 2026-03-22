MARGO ?= ../../build/margo
CC ?= cc
CFLAGS ?= -O3 -march=native -std=c11 -Wall -Wextra

all: bfs_c bfs_margo graph_gen

bfs_c: bfs_c.c
	$(CC) $(CFLAGS) $< -o $@

bfs_margo: bfs_margo.margo $(MARGO)
	$(MARGO) build $< -o $@

graph_gen: graph_gen.c
	$(CC) $(CFLAGS) $< -o $@

bench: all
	python3 bench.py

clean:
	rm -f bfs_c bfs_margo graph_gen graph.bin

.PHONY: all bench clean
