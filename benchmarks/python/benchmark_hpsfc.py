"""
Benchmark: HP-SFC lookup throughput vs baseline methods.

This verifies that the HP-SFC sparse-code fingerprint table provides superior
or competitive base-key lookup throughput vs binary search while maintaining
100% rank correctness.
"""

from __future__ import annotations

import json
import os
import random
import time
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from hli.datasets import generate_scalable_tree, generate_dns_paths, generate_json_paths
from hli.hpsfc import HPSFCTable, HPSFCRankTransportIndex
from hli.rank_transport import RankTransportIndex
from hli.poincare_embedding import embed_tree_poincare
from hli.baselines import PiecewiseLinearIndex
from hli.tree_generator import TreeNode
from bisect import bisect_left


RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results_q1")


def build_base_model(base_keys, epsilon=64):
    model = PiecewiseLinearIndex(epsilon=epsilon)
    model.build(base_keys)
    predictions = {key: model.predict_position(key) for key in base_keys}
    return predictions


def benchmark_dataset(name: str, paths: list[str], n_base: int = 3000):
    print(f"\n{'=' * 70}")
    print(f"HP-SFC Benchmark: {name}")
    print(f"{'=' * 70}")

    base_keys = sorted(set(paths))[:n_base]
    predicted = build_base_model(base_keys)

    # Build indices
    t0 = time.perf_counter()
    rt_index = RankTransportIndex(base_keys, base_epsilon=64)
    rt_build = (time.perf_counter() - t0) * 1000

    t0 = time.perf_counter()
    hpsfc_index = HPSFCRankTransportIndex(base_keys, base_epsilon=64)
    hpsfc_build = (time.perf_counter() - t0) * 1000

    # Correctness check
    errors = 0
    for key in base_keys:
        got = hpsfc_index.lookup(key)
        exp = hpsfc_index.exact_rank(key)
        if got != exp:
            errors += 1
    print(f"  Correctness: {len(base_keys) - errors}/{len(base_keys)} correct")

    # Throughput: binary search (rt_index.lookup) vs HP-SFC (hpsfc_index.lookup)
    lookup_keys = base_keys.copy()
    random.shuffle(lookup_keys)

    N_REPS = 5
    bs_times = []
    hpsfc_times = []
    for _ in range(N_REPS):
        t0 = time.perf_counter()
        for key in lookup_keys:
            rt_index.lookup(key, predicted[key])
        bs_times.append(time.perf_counter() - t0)

    for _ in range(N_REPS):
        t0 = time.perf_counter()
        for key in lookup_keys:
            hpsfc_index.lookup(key)
        hpsfc_times.append(time.perf_counter() - t0)

    bs_median = np.median(bs_times)
    hpsfc_median = np.median(hpsfc_times)

    bs_throughput = len(lookup_keys) / bs_median / 1e6
    hpsfc_throughput = len(lookup_keys) / hpsfc_median / 1e6

    speedup = bs_median / hpsfc_median

    # HP-SFC probe stats
    stats = hpsfc_index.hpsfc_stats()

    print(f"  Build time  -> RT-only: {rt_build:.2f} ms  |  HP-SFC: {hpsfc_build:.2f} ms")
    print(f"  Lookup throughput -> Binary search: {bs_throughput:.3f} Mops/s  |  HP-SFC: {hpsfc_throughput:.3f} Mops/s")
    print(f"  HP-SFC speedup: {speedup:.2f}x")
    print(f"  HP-SFC probe stats: avg={stats['avg_probes']:.2f}, max={stats['max_probes']}, load={stats['load_factor']:.3f}")

    return {
        "dataset": name,
        "n_base": len(base_keys),
        "errors": errors,
        "build_rt_ms": rt_build,
        "build_hpsfc_ms": hpsfc_build,
        "lookup_bs_throughput_mops": bs_throughput,
        "lookup_hpsfc_throughput_mops": hpsfc_throughput,
        "speedup": speedup,
        "probe_stats": stats,
    }


def plot_results(results: list[dict]):
    os.makedirs(RESULTS_DIR, exist_ok=True)
    
    names = [r["dataset"] for r in results]
    bs_tp = [r["lookup_bs_throughput_mops"] for r in results]
    hpsfc_tp = [r["lookup_hpsfc_throughput_mops"] for r in results]

    x = np.arange(len(names))
    width = 0.35

    fig, ax = plt.subplots(figsize=(10, 5), dpi=150)
    bars1 = ax.bar(x - width/2, bs_tp, width, label="Binary Search (baseline)", color="#495057", alpha=0.85)
    bars2 = ax.bar(x + width/2, hpsfc_tp, width, label="HP-SFC (sparse-code fingerprint)", color="#087f5b", alpha=0.85)

    for bar in bars1:
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.01,
                f"{bar.get_height():.2f}", ha="center", va="bottom", fontsize=9)
    for bar in bars2:
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.01,
                f"{bar.get_height():.2f}", ha="center", va="bottom", fontsize=9)

    ax.set_xlabel("Dataset", fontsize=12)
    ax.set_ylabel("Lookup Throughput (Mops/sec)", fontsize=12)
    ax.set_title("HP-SFC vs Binary Search Lookup Throughput", fontsize=14, fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels(names, fontsize=11)
    ax.legend(fontsize=10)
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    
    path = os.path.join(RESULTS_DIR, "hpsfc_vs_binary_search.png")
    plt.savefig(path, bbox_inches="tight")
    plt.close()
    print(f"\nPlot saved: {path}")


def main():
    random.seed(42)
    np.random.seed(42)

    datasets = {
        "Synthetic hierarchy": generate_scalable_tree(4000, max_depth=8, seed=11),
        "DNS paths": generate_dns_paths(4000, seed=12),
        "JSON paths": generate_json_paths(4000, seed=13),
    }

    all_results = []
    for name, paths in datasets.items():
        result = benchmark_dataset(name, paths, n_base=3000)
        all_results.append(result)

    plot_results(all_results)

    out = os.path.join(RESULTS_DIR, "hpsfc_benchmark_results.json")
    with open(out, "w") as f:
        json.dump(all_results, f, indent=2)
    print(f"\nResults saved: {out}")


if __name__ == "__main__":
    main()
