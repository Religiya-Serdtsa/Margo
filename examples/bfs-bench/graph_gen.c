#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

static uint32_t next_u32(uint64_t *state) {
    return (uint32_t)(next_random(state) >> 32);
}

static int write_graph(const char *path, uint32_t nodes, uint64_t edges, uint64_t seed) {
    if (nodes == 0 && edges > 0) {
        fprintf(stderr, "Cannot create edges without nodes.\n");
        return -1;
    }

    uint32_t *degree = (uint32_t *)calloc(nodes ? nodes : 1u, sizeof(uint32_t));
    if (!degree) {
        fprintf(stderr, "Failed to allocate degree array.\n");
        return -1;
    }

    uint64_t rng = seed;
    for (uint64_t i = 0; i < edges; ++i) {
        uint32_t src = nodes ? (next_u32(&rng) % nodes) : 0u;
        (void)(next_u32(&rng) % (nodes ? nodes : 1u));
        if (nodes) {
            degree[src]++;
        }
    }

    uint64_t *offsets = (uint64_t *)malloc(((size_t)nodes + 1u) * sizeof(uint64_t));
    if (!offsets) {
        fprintf(stderr, "Failed to allocate offsets array.\n");
        free(degree);
        return -1;
    }

    uint64_t cursor_total = 0;
    for (uint32_t i = 0; i < nodes; ++i) {
        offsets[i] = cursor_total;
        cursor_total += degree[i];
    }
    offsets[nodes] = cursor_total;

    if (cursor_total != edges) {
        fprintf(stderr, "Degree scan mismatch (expected %llu edges, saw %llu)\n",
                (unsigned long long)edges, (unsigned long long)cursor_total);
        free(degree);
        free(offsets);
        return -1;
    }

    uint32_t *edge_list = edges ? (uint32_t *)malloc(edges * sizeof(uint32_t)) : NULL;
    uint32_t *cursor = (uint32_t *)calloc(nodes ? nodes : 1u, sizeof(uint32_t));
    if ((edges && !edge_list) || !cursor) {
        fprintf(stderr, "Failed to allocate edge payload buffers.\n");
        free(degree);
        free(offsets);
        free(edge_list);
        free(cursor);
        return -1;
    }

    uint64_t rng_fill = seed;
    for (uint64_t i = 0; i < edges; ++i) {
        uint32_t src = nodes ? (next_u32(&rng_fill) % nodes) : 0u;
        uint32_t dst = nodes ? (next_u32(&rng_fill) % nodes) : 0u;
        if (!nodes) {
            continue;
        }
        uint64_t pos = offsets[src] + cursor[src];
        edge_list[pos] = dst;
        cursor[src]++;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Failed to open %s for writing: %s\n", path, strerror(errno));
        free(degree);
        free(offsets);
        free(edge_list);
        free(cursor);
        return -1;
    }

    if (fwrite(&nodes, sizeof(nodes), 1, file) != 1 ||
        fwrite(&edges, sizeof(edges), 1, file) != 1 ||
        fwrite(offsets, sizeof(uint64_t), (size_t)nodes + 1u, file) != (size_t)nodes + 1u ||
        (edges > 0 && fwrite(edge_list, sizeof(uint32_t), edges, file) != edges)) {
        fprintf(stderr, "Failed to write graph to %s\n", path);
        fclose(file);
        free(degree);
        free(offsets);
        free(edge_list);
        free(cursor);
        return -1;
    }

    fclose(file);
    free(degree);
    free(offsets);
    free(edge_list);
    free(cursor);

    printf("Graph written to %s (nodes=%u, edges=%llu, seed=%llu)\n", path, nodes,
           (unsigned long long)edges, (unsigned long long)seed);
    return 0;
}

static uint64_t parse_u64(const char *arg, const char *name) {
    errno = 0;
    char *end = NULL;
    uint64_t value = strtoull(arg, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        fprintf(stderr, "Invalid %s: %s\n", name, arg);
        exit(1);
    }
    return value;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "graph.bin";
    uint32_t nodes = 4000000000u;
    uint64_t edges = 40000000000ULL;
    uint64_t seed = 123456789ULL;

    if (argc > 2) {
        nodes = (uint32_t)parse_u64(argv[2], "node count");
    }
    if (argc > 3) {
        edges = parse_u64(argv[3], "edge count");
    }
    if (argc > 4) {
        seed = parse_u64(argv[4], "seed");
    }

    if (write_graph(path, nodes, edges, seed) != 0) {
        return 1;
    }
    return 0;
}
