#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Graph {
    uint32_t num_nodes;
    uint64_t num_edges;
    uint64_t *offsets;
    uint32_t *edges;
};

struct BFSStats {
    uint64_t visited_nodes;
    uint64_t distance_sum;
    int32_t max_distance;
};

static void free_graph(struct Graph *graph) {
    if (!graph) {
        return;
    }
    free(graph->offsets);
    free(graph->edges);
    graph->offsets = NULL;
    graph->edges = NULL;
    graph->num_nodes = 0;
    graph->num_edges = 0;
}

static int load_graph(const char *path, struct Graph *graph) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    memset(graph, 0, sizeof(*graph));

    uint32_t nodes = 0;
    uint64_t edges = 0;
    if (fread(&nodes, sizeof(nodes), 1, file) != 1 ||
        fread(&edges, sizeof(edges), 1, file) != 1) {
        fprintf(stderr, "Failed to read graph header from %s\n", path);
        fclose(file);
        return -1;
    }

    size_t offsets_count = (size_t)nodes + 1u;
    uint64_t *offsets = (uint64_t *)malloc(offsets_count * sizeof(uint64_t));
    uint32_t *edge_list = (uint32_t *)malloc(edges * sizeof(uint32_t));
    if (!offsets || !edge_list) {
        fprintf(stderr, "Out of memory while loading %s\n", path);
        free(offsets);
        free(edge_list);
        fclose(file);
        return -1;
    }

    if (fread(offsets, sizeof(uint64_t), offsets_count, file) != offsets_count ||
        fread(edge_list, sizeof(uint32_t), edges, file) != edges) {
        fprintf(stderr, "Failed to read graph payload from %s\n", path);
        free(offsets);
        free(edge_list);
        fclose(file);
        return -1;
    }
    if (offsets[offsets_count - 1] != edges) {
        fprintf(stderr, "Graph file %s is corrupted (edge count mismatch)\n", path);
        free(offsets);
        free(edge_list);
        fclose(file);
        return -1;
    }

    fclose(file);

    graph->num_nodes = nodes;
    graph->num_edges = edges;
    graph->offsets = offsets;
    graph->edges = edge_list;
    return 0;
}

static int run_bfs(const struct Graph *graph, struct BFSStats *stats) {
    if (graph->num_nodes == 0) {
        fprintf(stderr, "Graph is empty.\n");
        return -1;
    }

    uint32_t node_count = graph->num_nodes;
    uint32_t *queue = (uint32_t *)malloc((size_t)node_count * sizeof(uint32_t));
    int32_t *distances = (int32_t *)malloc((size_t)node_count * sizeof(int32_t));
    if (!queue || !distances) {
        fprintf(stderr, "Out of memory allocating BFS buffers.\n");
        free(queue);
        free(distances);
        return -1;
    }

    for (uint32_t i = 0; i < node_count; ++i) {
        distances[i] = -1;
    }

    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t size = 0;

    queue[tail] = 0;
    tail = (tail + 1u == node_count) ? 0u : (tail + 1u);
    size = 1;
    distances[0] = 0;

    uint64_t visited = 1;
    uint64_t distance_sum = 0;
    int32_t max_distance = 0;

    int rc = 0;

    while (size > 0) {
        uint32_t node = queue[head];
        head = (head + 1u == node_count) ? 0u : (head + 1u);
        size--;
        int32_t base_distance = distances[node];

        uint64_t begin = graph->offsets[node];
        uint64_t end = graph->offsets[node + 1u];
        for (uint64_t idx = begin; idx < end; ++idx) {
            uint32_t neighbor = graph->edges[idx];
            if (distances[neighbor] != -1) {
                continue;
            }
            int32_t next_distance = base_distance + 1;
            distances[neighbor] = next_distance;
            if (size == node_count) {
                fprintf(stderr, "Queue overflow during BFS traversal\n");
                rc = -1;
                goto cleanup;
            }
            queue[tail] = neighbor;
            tail = (tail + 1u == node_count) ? 0u : (tail + 1u);
            size++;
            visited++;
            distance_sum += (uint64_t)next_distance;
            if (next_distance > max_distance) {
                max_distance = next_distance;
            }
        }
    }

cleanup:
    free(queue);
    free(distances);

    if (rc != 0) {
        return rc;
    }

    stats->visited_nodes = visited;
    stats->distance_sum = distance_sum;
    stats->max_distance = max_distance;
    return 0;
}

int main(int argc, char **argv) {
    const char *graph_path = (argc > 1) ? argv[1] : "graph.bin";

    struct Graph graph;
    if (load_graph(graph_path, &graph) != 0) {
        return 1;
    }

    struct BFSStats stats;
    if (run_bfs(&graph, &stats) != 0) {
        free_graph(&graph);
        return 1;
    }

    printf("=== BFS (C -O3) ===\n");
    printf("Nodes: %u  Edges: %llu\n", graph.num_nodes, (unsigned long long)graph.num_edges);
    printf("Visited nodes: %llu\n", (unsigned long long)stats.visited_nodes);
    printf("Distance sum: %llu  Max distance: %d\n", (unsigned long long)stats.distance_sum,
           stats.max_distance);

    free_graph(&graph);
    return 0;
}
