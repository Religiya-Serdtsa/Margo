# BFS Benchmark (Margo vs. C)

Large directed graphs stress both Margo and an optimized C implementation with identical data. Each program loads an adjacency list that stores 1,000,000 nodes and 10,000,000 edges in contiguous buffers, runs a queue-based BFS starting at node 0, and prints a short traversal summary. The `bench.py` helper executes both binaries via `/usr/bin/time -v`, parses the resulting metrics, and prints a comparison focused on user time, RSS, page faults, and voluntary context switches.

## Contents

- `bfs_margo.margo` – original Margo BFS benchmark implementation, now using `@import matrix/core` for queue/distances so the traversal touches the same auto_matrix helpers showcased in other examples.
- `bfs_bench_safe.margo` – allocator-safe Margo BFS benchmark variant that also relies on `matrix/core` while keeping the input validation and guard rails in pure Margo (no raw C headers).
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
make                          # builds bfs_margo, bfs_bench_safe, bfs_c, graph_gen
python3 bench.py              # default: compares bfs_bench_safe vs bfs_c with /usr/bin/time -v
python3 bench.py --margo-bin bfs_margo   # compare original bfs_margo vs bfs_c
```

`bench.py` accepts a few quality-of-life flags:

- `--graph PATH` – use or create a different graph file (default `graph.bin`).
- `--nodes N`, `--edges E`, `--seed S` – parameters passed to `graph_gen` when the graph is missing or `--regenerate` is set.
- `--regenerate` – force a fresh graph build even if the file already exists.
- `--margo-bin {bfs_bench_safe,bfs_margo}` – choose which Margo binary to compare against C (default `bfs_bench_safe`).
- `--skip-build` – reuse existing binaries instead of invoking `make`.

The generator can also be invoked manually:

```bash
./graph_gen graph.bin 1000000 10000000 123456789
```

After each run the script prints the stdout of both executables (node/edge counts, visited nodes, aggregate distance) followed by a tabular comparison of `/usr/bin/time -v` metrics so you can see how close Margo is to optimized C within the 5% user time target and whether its memory footprint diverges.

Clean artifacts with `make clean` (this removes the binaries and `graph.bin`).
