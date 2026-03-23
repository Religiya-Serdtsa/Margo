#!/usr/bin/env python3
"""Run BFS benchmarks for Margo vs. C with /usr/bin/time -v."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_GRAPH = "graph.bin"
DEFAULT_NODES = 1_000_000
DEFAULT_EDGES = 10_000_000
DEFAULT_SEED = 123_456_789

METRIC_FIELDS = [
    ("User time (seconds)", float),
    ("Maximum resident set size (kbytes)", int),
    ("Minor (reclaiming a frame) page faults", int),
    ("Voluntary context switches", int),
]

VALUE_WIDTH = 18


def run_subprocess(cmd: list[str], cwd: Path = ROOT, capture: bool = False) -> subprocess.CompletedProcess:
    kwargs = {
        "cwd": str(cwd),
        "text": True,
    }
    if capture:
        kwargs["capture_output"] = True
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(
            f"Command {' '.join(cmd)} failed with exit code {result.returncode}\n{result.stderr if capture else ''}"
        )
    return result


def ensure_build(skip: bool) -> None:
    if skip:
        return
    run_subprocess(["make", "bfs_c", "bfs_margo", "bfs_bench_safe", "graph_gen"])


def ensure_graph(path: Path, nodes: int, edges: int, seed: int, regenerate: bool) -> None:
    if not regenerate and path.exists():
        return
    if not path.parent.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["./graph_gen", str(path), str(nodes), str(edges), str(seed)]
    run_subprocess(cmd)


def parse_metrics(stderr: str) -> dict[str, float | int]:
    metrics: dict[str, float | int] = {}
    for line in stderr.splitlines():
        if ":" not in line:
            continue
        name, raw_value = line.split(":", 1)
        key = name.strip()
        for metric_name, caster in METRIC_FIELDS:
            if key == metric_name:
                value_str = raw_value.strip()
                try:
                    metrics[key] = caster(value_str)
                except ValueError as exc:  # pragma: no cover - defensive
                    raise RuntimeError(f"Failed to parse {key!r}: {value_str!r}") from exc
    missing = [name for name, _ in METRIC_FIELDS if name not in metrics]
    if missing:
        raise RuntimeError(f"Missing metrics from /usr/bin/time output: {', '.join(missing)}")
    return metrics


def run_timed(binary: str, graph_path: Path) -> tuple[dict[str, float | int], str]:
    time_path = shutil.which("/usr/bin/time") or "/usr/bin/time"
    cmd = [time_path, "-v", binary, str(graph_path)]
    result = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"Command {' '.join(cmd)} failed with exit code {result.returncode}\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}"
        )
    metrics = parse_metrics(result.stderr)
    return metrics, result.stdout.strip()


def format_value(name: str, value: float | int) -> str:
    if name == "User time (seconds)":
        return f"{value:.3f}s"
    if name == "Maximum resident set size (kbytes)":
        return f"{int(value):,} kB"
    return f"{int(value):,}"


def format_delta(name: str, delta: float) -> str:
    if name == "User time (seconds)":
        return f"{delta:+.3f}s"
    return f"{int(delta):+d}"


def format_percent(base: float, delta: float) -> str:
    if base == 0:
        return "n/a"
    percent = (delta / base) * 100.0
    return f"{percent:+.2f}%"


def compare_metrics(margo: dict[str, float | int], c_metrics: dict[str, float | int]) -> str:
    lines = []
    header = f"{'Metric':<40}{'Margo':>{VALUE_WIDTH}}{'C (-O3)':>{VALUE_WIDTH}}{'Delta':>{VALUE_WIDTH}}{'% vs C':>10}"
    lines.append(header)
    lines.append("-" * len(header))
    for name, _ in METRIC_FIELDS:
        m_value = margo[name]
        c_value = c_metrics[name]
        delta = float(m_value) - float(c_value)
        lines.append(
            f"{name:<40}"
            f"{format_value(name, m_value):>{VALUE_WIDTH}}"
            f"{format_value(name, c_value):>{VALUE_WIDTH}}"
            f"{format_delta(name, delta):>{VALUE_WIDTH}}"
            f"{format_percent(float(c_value), delta):>10}"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph", default=DEFAULT_GRAPH, help="Graph file path")
    parser.add_argument("--nodes", type=int, default=DEFAULT_NODES, help="Number of nodes for generation")
    parser.add_argument("--edges", type=int, default=DEFAULT_EDGES, help="Number of edges for generation")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="Seed used by graph generator")
    parser.add_argument("--regenerate", action="store_true", help="Force graph regeneration before benchmarking")
    parser.add_argument(
        "--margo-bin",
        choices=["bfs_margo", "bfs_bench_safe"],
        default="bfs_bench_safe",
        help="Margo benchmark binary to run (default: bfs_bench_safe)",
    )
    parser.add_argument("--skip-build", action="store_true", help="Skip invoking make before running the benchmarks")
    args = parser.parse_args()

    graph_path = (ROOT / args.graph).resolve()

    ensure_build(args.skip_build)
    ensure_graph(graph_path, args.nodes, args.edges, args.seed, args.regenerate)

    print(f"Benchmarking with graph: {graph_path}")

    margo_binary = f"./{args.margo_bin}"
    margo_metrics, margo_stdout = run_timed(margo_binary, graph_path)
    c_metrics, c_stdout = run_timed("./bfs_c", graph_path)

    if margo_stdout:
        print(f"\n[Margo stdout: {args.margo_bin}]")
        print(margo_stdout)
    if c_stdout:
        print("\n[C stdout]")
        print(c_stdout)

    print("\nComparison (/usr/bin/time -v):")
    print(compare_metrics(margo_metrics, c_metrics))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
