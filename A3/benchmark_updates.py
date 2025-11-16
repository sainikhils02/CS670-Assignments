#!/usr/bin/env python3
"""Benchmark runtime per query for Assignment 3 updates.

The script automates the following loop:
1. Regenerate shares/queries with a chosen (num_users, num_items, num_queries).
2. Run only the MPC parties (p2, p1, p0) under docker compose and time the run.
3. Store the average wall-clock time per query and emit comparison plots for
   (a) varying items at fixed user counts and (b) varying users at fixed item counts.

Usage examples
--------------
    # Run full benchmark with defaults (may take several minutes)
    python benchmark_updates.py

    # Dry-run mode: only prints docker commands without executing them
    python benchmark_updates.py --dry-run

    # Reduce repetitions to shorten runtime
    python benchmark_updates.py --repetitions 1
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List

import matplotlib.pyplot as plt

A3_ROOT = Path(__file__).resolve().parent
DEFAULT_NUM_QUERIES = 8
DEFAULT_REPETITIONS = 2
RESULTS_JSON = A3_ROOT / "benchmark_results.json"
PLOTS_DIR = A3_ROOT / "plots"

ITEM_SWEEPS = [
    {"label": "small users (16)", "users": 16, "items": [16, 32, 64, 128, 256, 1024]},
    {"label": "moderate users (128)", "users": 128, "items": [16, 32, 64, 128, 256, 1024]},
    {"label": "large users (1024)", "users": 1024, "items": [16, 32, 64, 128, 256, 1024]},
]

USER_SWEEPS = [
    {"label": "small items (16)", "items": 16, "users": [16, 32, 64, 128, 256, 1024]},
    {"label": "moderate items (128)", "items":128, "users": [16, 32, 64, 128, 256, 1024]},
    {"label": "large items (1024)", "items": 1024, "users": [16, 32, 64, 128, 256, 1024]},
]

COMPOSE_BASE = ["docker", "compose"]


def run_command(args: List[str], *, dry_run: bool, check: bool = True) -> None:
    """Run a subprocess inside A3_ROOT, printing the command beforehand."""

    printable = " ".join(args)
    print(f"[cmd] {printable}")
    if dry_run:
        return
    subprocess.run(args, cwd=A3_ROOT, check=check)


def compose_down(*, dry_run: bool) -> None:
    run_command(COMPOSE_BASE + ["down"], dry_run=dry_run, check=False)


def regenerate_data(users: int, items: int, queries: int, *, dry_run: bool) -> None:
    run_command(
        COMPOSE_BASE
        + [
            "run",
            "--rm",
            "gen",
            "./gen_queries",
            str(users),
            str(items),
            str(queries),
        ],
        dry_run=dry_run,
    )


def run_protocol(*, dry_run: bool) -> float:
    cmd = COMPOSE_BASE + [
        "up",
        "--force-recreate",
        "--abort-on-container-exit",
        "p2",
        "p1",
        "p0",
    ]
    start = time.perf_counter()
    run_command(cmd, dry_run=dry_run)
    duration = time.perf_counter() - start
    run_command(COMPOSE_BASE + ["rm", "-f", "p0", "p1", "p2"], dry_run=dry_run, check=False)
    return duration if not dry_run else 0.0


def benchmark_point(
    *,
    users: int,
    items: int,
    queries: int,
    repetitions: int,
    dry_run: bool,
) -> Dict[str, float]:
    durations: List[float] = []
    for rep in range(repetitions):
        print(f"\n[info] Run {rep + 1}/{repetitions} for users={users}, items={items}")
        regenerate_data(users, items, queries, dry_run=dry_run)
        duration = run_protocol(dry_run=dry_run)
        durations.append(duration)
    avg_runtime = sum(durations) / max(len(durations), 1)
    per_query = avg_runtime / queries if queries else 0.0
    return {
        "users": users,
        "items": items,
        "queries": queries,
        "avg_runtime_sec": avg_runtime,
        "per_query_ms": per_query * 1000.0,
    }


def benchmark_sweeps(
    sweeps: Iterable[Dict[str, object]],
    vary_key: str,
    queries: int,
    repetitions: int,
    dry_run: bool,
) -> List[Dict[str, float]]:
    records: List[Dict[str, float]] = []
    for sweep in sweeps:
        fixed_label = sweep["label"]
        fixed_value = sweep["users"] if "users" in sweep and vary_key == "items" else sweep["items"]
        print(f"\n[info] Sweep: {fixed_label}, fixed={fixed_value}, varying {vary_key}")
        for value in sweep[vary_key]:
            params = {
                "users": sweep.get("users", value if vary_key == "users" else fixed_value),
                "items": sweep.get("items", value if vary_key == "items" else fixed_value),
            }
            params[vary_key] = value
            result = benchmark_point(
                users=params["users"],
                items=params["items"],
                queries=queries,
                repetitions=repetitions,
                dry_run=dry_run,
            )
            result["sweep_label"] = fixed_label
            records.append(result)
    return records


def plot_results(
    records: List[Dict[str, float]],
    *,
    x_key: str,
    group_key: str,
    xlabel: str,
    title: str,
    output_name: str,
) -> None:
    if not records:
        print(f"[warn] No data to plot for {title}")
        return

    PLOTS_DIR.mkdir(exist_ok=True)
    grouped: Dict[str, List[Dict[str, float]]] = defaultdict(list)
    for rec in records:
        grouped[rec[group_key]].append(rec)

    plt.figure(figsize=(7.5, 5.0))
    for label, points in grouped.items():
        points_sorted = sorted(points, key=lambda x: x[x_key])
        xs = [pt[x_key] for pt in points_sorted]
        ys = [pt["per_query_ms"] for pt in points_sorted]
        plt.plot(xs, ys, marker="o", label=label)

    plt.xlabel(xlabel)
    plt.ylabel("Time per query (ms)")
    plt.title(title)
    plt.grid(True, linestyle="--", alpha=0.3)
    plt.legend()
    outfile = PLOTS_DIR / output_name
    plt.tight_layout()
    plt.savefig(outfile)
    print(f"[info] Wrote {outfile}")
    plt.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark user/item update latency per query.")
    parser.add_argument(
        "--queries",
        type=int,
        default=DEFAULT_NUM_QUERIES,
        help="Number of queries per run (default: %(default)s)",
    )
    parser.add_argument(
        "--repetitions",
        type=int,
        default=DEFAULT_REPETITIONS,
        help="How many times to repeat each parameter point (default: %(default)s)",
    )
    parser.add_argument(
        "--skip-plots",
        action="store_true",
        help="Collect data but skip matplotlib plots.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only print docker commands without executing them (useful for smoke tests)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    compose_down(dry_run=args.dry_run)

    try:
        items_records = benchmark_sweeps(
            ITEM_SWEEPS,
            vary_key="items",
            queries=args.queries,
            repetitions=args.repetitions,
            dry_run=args.dry_run,
        )
        users_records = benchmark_sweeps(
            USER_SWEEPS,
            vary_key="users",
            queries=args.queries,
            repetitions=args.repetitions,
            dry_run=args.dry_run,
        )
    finally:
        compose_down(dry_run=args.dry_run)

    all_results = {
        "metadata": {
            "queries_per_run": args.queries,
            "repetitions": args.repetitions,
            "dry_run": args.dry_run,
        },
        "vary_items": items_records,
        "vary_users": users_records,
    }
    RESULTS_JSON.write_text(json.dumps(all_results, indent=2))
    print(f"[info] Wrote {RESULTS_JSON}")

    if not args.skip_plots:
        plot_results(
            items_records,
            x_key="items",
            group_key="sweep_label",
            xlabel="Number of items",
            title="Item profile update latency",
            output_name="time_vs_items.png",
        )
        plot_results(
            users_records,
            x_key="users",
            group_key="sweep_label",
            xlabel="Number of users",
            title="User profile update latency",
            output_name="time_vs_users.png",
        )


if __name__ == "__main__":
    if sys.version_info < (3, 9):
        raise SystemExit("Python 3.9+ is required.")
    main()
