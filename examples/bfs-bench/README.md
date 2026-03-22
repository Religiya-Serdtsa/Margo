# BFS Benchmark (Margo vs. C)

Large directed graphs stress both Margo and an optimized C implementation with identical data. Each program loads an adjacency list that stores 1,000,000 nodes and 10,000,000 edges in contiguous buffers, runs a queue-based BFS starting at node 0, and prints a short traversal summary. The `bench.py` helper executes both binaries via `/usr/bin/time -v`, parses the resulting metrics, and prints a comparison focused on user time, RSS, page faults, and voluntary context switches.

## Contents

- `bfs_margo.margo` – native Margo BFS implementation that leans on the built-in libttak allocator (no extra `@import c/ttak/...` needed) plus a manual circular queue and 32-bit distances.
- `bfs_c.c` – reference C implementation compiled with `-O3`.
- `graph_gen.c` – deterministic graph generator that produces a shared binary adjacency file.
- `bench.py` – automation script that runs `/usr/bin/time -v` for both binaries and compares metrics.
- `Makefile` – builds `bfs_margo`, `bfs_c`, and `graph_gen` (defaults to `../../build/margo` and `cc -O3`).

## Graph format

`graph_gen` emits a binary file with the following layout:

1. `uint32_t num_nodes`
2. `uint64_t num_edges`
3. `uint64_t offsets[num_nodes + 1]` – prefix sum of outgoing degrees
4. `uint32_t edges[num_edges]` – adjacency payload stored as a single contiguous block

All values are written in the host endianness (little-endian on Linux). Both BFS binaries read this format directly, so the generator must be run before benchmarking.

## Usage

```bash
cd examples/bfs-bench
make                          # builds bfs_margo, bfs_c, graph_gen
python3 bench.py              # ensures graph.bin, runs both binaries with /usr/bin/time -v
```

`bench.py` accepts a few quality-of-life flags:

- `--graph PATH` – use or create a different graph file (default `graph.bin`).
- `--nodes N`, `--edges E`, `--seed S` – parameters passed to `graph_gen` when the graph is missing or `--regenerate` is set.
- `--regenerate` – force a fresh graph build even if the file already exists.
- `--skip-build` – reuse existing binaries instead of invoking `make`.

The generator can also be invoked manually:

```bash
./graph_gen graph.bin 1000000 10000000 123456789
```

After each run the script prints the stdout of both executables (node/edge counts, visited nodes, aggregate distance) followed by a tabular comparison of `/usr/bin/time -v` metrics so you can see how close Margo is to optimized C within the 5% user time target and whether its memory footprint diverges.

Clean artifacts with `make clean` (this removes the binaries and `graph.bin`).
