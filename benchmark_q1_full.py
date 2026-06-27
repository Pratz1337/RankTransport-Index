"""
Q1-grade full benchmark suite for HRT-LI.

Covers:
1. Scalability: N ∈ {10K, 50K, 100K, 500K} keys
2. Workload mixes: Read-only (0% write), Mixed (50%), Write-heavy (95% write)
3. Range query throughput vs competitors
4. Statistical rigor: 5 independent trials, 95% bootstrap CI

This generates the main experimental results for the paper's
Section 6 (Experimental Evaluation).
"""

from __future__ import annotations

import json
import math
import os
import random
import time
from collections import defaultdict
from typing import Any

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

from hli.datasets import (
    generate_scalable_tree,
    generate_dns_paths,
    generate_json_paths,
    generate_package_manager_paths,
    generate_url_paths,
    load_filesystem_paths,
    load_linux_kernel_paths,
    load_wikipedia_categories,
    IntegerKeyWrapper,
)
from hli.hpsfc import HPSFCRankTransportIndex
from hli.rank_transport import RankTransportIndex
from hli.range_query import RangeQueryIndex
from hli.baselines import (
    PiecewiseLinearIndex,
    GappedArrayIndex,
    AdaptiveRadixTree,
    BPlusTreeIndex
)


RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results_q1")
N_TRIALS = 5
EPSILON = 64
RANDOM_SEED = 42


# ─── Bootstrap CI ─────────────────────────────────────────────────────────────

def bootstrap_ci(values: list[float], n_boot: int = 2000, alpha: float = 0.05):
    """Compute 95% bootstrap confidence interval."""
    arr = np.array(values)
    if len(arr) < 2:
        return float(arr[0]), float(arr[0]), float(arr[0])
    boot_means = np.array([np.mean(arr[np.random.randint(0, len(arr), len(arr))]) for _ in range(n_boot)])
    lo = np.percentile(boot_means, 100 * alpha / 2)
    hi = np.percentile(boot_means, 100 * (1 - alpha / 2))
    return float(np.mean(arr)), float(lo), float(hi)


# ─── Dataset generators ────────────────────────────────────────────────────────

def get_dataset(name: str, n: int, seed: int = RANDOM_SEED) -> list[str]:
    if name == "synthetic":
        return sorted(set(generate_scalable_tree(int(n * 1.5), max_depth=10, seed=seed)))[:n]
    elif name == "dns":
        return sorted(set(generate_dns_paths(int(n * 1.5), seed=seed)))[:n]
    elif name == "json":
        return sorted(set(generate_json_paths(int(n * 1.5), seed=seed)))[:n]
    elif name == "filesystem":
        return sorted(set(load_filesystem_paths(os.path.expanduser("~"), int(n * 1.5))))[:n]
    elif name == "url":
        return sorted(set(generate_url_paths(int(n * 1.5), seed=seed)))[:n]
    elif name == "packages":
        return sorted(set(generate_package_manager_paths(int(n * 1.5), seed=seed)))[:n]
    elif name == "linux":
        return sorted(set(load_linux_kernel_paths(max_paths=int(n * 1.5))))[:n]
    elif name == "wikipedia":
        return sorted(set(load_wikipedia_categories(int(n * 1.5), seed=seed)))[:n]
    elif name == "integer":
        raw = [random.randint(0, 10**9) for _ in range(int(n * 1.1))]
        return IntegerKeyWrapper(raw).get_keys()[:n]
    raise ValueError(f"Unknown dataset: {name}")


# ─── Build base model predictions ─────────────────────────────────────────────

def build_predictions(base_keys: list[str]) -> dict[str, int]:
    model = PiecewiseLinearIndex(epsilon=EPSILON)
    model.build(base_keys)
    return {k: model.predict_position(k) for k in base_keys}


# ─── Throughput measurement ────────────────────────────────────────────────────

def measure_lookup_throughput(
    index: HPSFCRankTransportIndex,
    keys: list[str],
    n_trials: int = N_TRIALS,
) -> tuple[float, float, float]:
    """Returns (mean_mops, ci_lo_mops, ci_hi_mops)."""
    shuffled = keys.copy()
    random.shuffle(shuffled)
    times = []
    for _ in range(n_trials):
        t0 = time.perf_counter()
        for k in shuffled:
            index.lookup(k)
        times.append((time.perf_counter() - t0))
    mops = [len(shuffled) / t / 1e6 for t in times]
    return bootstrap_ci(mops)


def measure_range_throughput(
    index: RangeQueryIndex,
    keys: list[str],
    n_trials: int = N_TRIALS,
) -> tuple[float, float, float]:
    """Range queries: random 1% range width."""
    n = len(keys)
    pairs = []
    for _ in range(min(1000, n // 10)):
        i = random.randint(0, n - 2)
        j = min(n - 1, i + max(1, n // 100))
        pairs.append((keys[i], keys[j]))
    times = []
    for _ in range(n_trials):
        t0 = time.perf_counter()
        for lo, hi in pairs:
            index.count_range(lo, hi)
        times.append(time.perf_counter() - t0)
    mops = [len(pairs) / t / 1e6 for t in times]
    return bootstrap_ci(mops)


def measure_mixed_workload(
    index: HPSFCRankTransportIndex,
    base_keys: list[str],
    write_ratio: float = 0.5,
    n_ops: int = 5000,
    n_trials: int = N_TRIALS,
) -> tuple[float, float, float]:
    """Simulate mixed read/write workload."""
    rng = random.Random(RANDOM_SEED)
    extra_keys = [base_keys[i] + "__w" for i in range(n_ops)]
    times = []
    for _ in range(n_trials):
        # Reset to clean state
        idx_copy = HPSFCRankTransportIndex(base_keys, base_epsilon=EPSILON)
        t0 = time.perf_counter()
        ei = 0
        for _ in range(n_ops):
            if rng.random() < write_ratio and ei < len(extra_keys):
                idx_copy.insert(extra_keys[ei])
                ei += 1
            else:
                k = rng.choice(base_keys)
                idx_copy.lookup(k)
        times.append(time.perf_counter() - t0)
    mops = [n_ops / t / 1e6 for t in times]
    return bootstrap_ci(mops)


# ─── Memory measurement ────────────────────────────────────────────────────────

def measure_memory(base_keys: list[str]) -> dict[str, float]:
    """Estimate memory usage of each component in KB."""
    import sys

    idx = HPSFCRankTransportIndex(base_keys, base_epsilon=EPSILON)
    # Approximate sizes
    base_size = sum(sys.getsizeof(k) for k in base_keys) / 1024
    hpsfc_size = sum(
        sys.getsizeof(s) for s in idx._hpsfc._table if s is not None
    ) / 1024 if hasattr(idx._hpsfc, '_table') else 0
    return {
        "base_keys_kb": round(base_size, 2),
        "hpsfc_table_kb": round(hpsfc_size, 2),
        "total_est_kb": round(base_size + hpsfc_size, 2),
    }


# ─── Main Experiment 1: Scalability ───────────────────────────────────────────

def run_scalability_experiment():
    print("\n" + "="*70)
    print("EXPERIMENT 1: Scalability (N ∈ {10K, 50K, 100K, 500K, 1M})")
    print("="*70)

    Ns = [10_000, 50_000, 100_000, 500_000, 1_000_000]
    dataset_names = ["synthetic", "filesystem", "url", "dns", "json", "packages", "integer"]
    results = defaultdict(list)

    for ds in dataset_names:
        print(f"\n  Dataset: {ds}")
        for N in Ns:
            keys = get_dataset(ds, N)
            if len(keys) < N:
                print(f"    N={N//1000}K: insufficient data ({len(keys)} keys), skipping")
                continue
            mean, lo, hi = measure_lookup_throughput(
                HPSFCRankTransportIndex(keys, EPSILON), keys
            )
            print(f"    N={N//1000:4d}K: {mean:.3f} Mops/s  [95% CI: {lo:.3f}, {hi:.3f}]")
            results[ds].append({
                "N": N,
                "mean_mops": mean,
                "ci_lo": lo,
                "ci_hi": hi,
            })

    return dict(results)


# ─── Main Experiment 2: Workload Mix ──────────────────────────────────────────

def run_workload_experiment():
    print("\n" + "="*70)
    print("EXPERIMENT 2: Workload Mix Analysis")
    print("="*70)

    workloads = [
        ("Read-only (0% write)", 0.0),
        ("Mixed (50% write)", 0.5),
        ("Write-heavy (95% write)", 0.95),
    ]
    N = 50_000
    dataset_names = ["synthetic", "filesystem", "url", "dns", "json", "packages"]
    all_results = {}

    for ds in dataset_names:
        keys = get_dataset(ds, N)
        if len(keys) < N:
            keys = keys  # use available
        print(f"\n  Dataset: {ds} ({len(keys)} keys)")
        ds_results = []
        for label, wr in workloads:
            mean, lo, hi = measure_mixed_workload(keys, keys, write_ratio=wr)
            print(f"    {label}: {mean:.3f} Mops/s  [95% CI: {lo:.3f}, {hi:.3f}]")
            ds_results.append({"workload": label, "write_ratio": wr, "mean_mops": mean, "ci_lo": lo, "ci_hi": hi})
        all_results[ds] = ds_results

    return all_results


# ─── Main Experiment 3: Range Query Throughput ────────────────────────────────

def run_range_experiment():
    print("\n" + "="*70)
    print("EXPERIMENT 3: Range Query Throughput")
    print("="*70)

    N = 50_000
    dataset_names = ["synthetic", "filesystem", "url", "dns", "json", "packages"]
    all_results = {}

    for ds in dataset_names:
        keys = get_dataset(ds, N)
        idx = RangeQueryIndex(keys, base_epsilon=EPSILON)
        mean, lo, hi = measure_range_throughput(idx, keys)
        print(f"  {ds}: {mean:.3f} Mops/s  [95% CI: {lo:.3f}, {hi:.3f}]")
        all_results[ds] = {"mean_mops": mean, "ci_lo": lo, "ci_hi": hi}

    return all_results


# ─── Plotting ─────────────────────────────────────────────────────────────────

def plot_scalability(results: dict):
    os.makedirs(RESULTS_DIR, exist_ok=True)
    fig, axes = plt.subplots(1, 3, figsize=(15, 5), dpi=150)
    
    colors = {
        "synthetic": "#087f5b",
        "filesystem": "#495057",
        "url": "#1971c2",
        "dns": "#862e9c",
        "json": "#e67700",
        "packages": "#5c940d",
        "integer": "#0b7285",
    }
    labels = {
        "synthetic": "Synthetic Tree",
        "filesystem": "Filesystem",
        "url": "URL Paths",
        "dns": "DNS Paths",
        "json": "JSON Paths",
        "packages": "Packages",
        "integer": "Integers",
    }

    for ax, ds in zip(axes, ["synthetic", "filesystem", "url"]):
        data = results.get(ds, [])
        if not data:
            continue
        Ns = [d["N"] / 1000 for d in data]
        means = [d["mean_mops"] for d in data]
        lo_err = [d["mean_mops"] - d["ci_lo"] for d in data]
        hi_err = [d["ci_hi"] - d["mean_mops"] for d in data]

        ax.errorbar(Ns, means, yerr=[lo_err, hi_err],
                    marker="o", linewidth=2, color=colors[ds],
                    capsize=5, capthick=2, markersize=8)
        ax.set_xlabel("N (thousands)", fontsize=12)
        ax.set_ylabel("Throughput (Mops/s)", fontsize=12)
        ax.set_title(f"{labels[ds]}", fontsize=13, fontweight="bold")
        ax.grid(alpha=0.3)
        ax.set_xscale("log")

    plt.suptitle("HRT-LI Scalability: Lookup Throughput vs Dataset Size",
                 fontsize=14, fontweight="bold", y=1.02)
    plt.tight_layout()
    path = os.path.join(RESULTS_DIR, "scalability_experiment.png")
    plt.savefig(path, bbox_inches="tight")
    plt.close()
    print(f"\nScalability plot saved: {path}")


def plot_workloads(results: dict):
    fig, ax = plt.subplots(figsize=(10, 5), dpi=150)
    dataset_names = list(results.keys())
    workload_labels = ["Read-only", "Mixed 50%", "Write-heavy 95%"]
    colors = ["#087f5b", "#1971c2", "#862e9c"]
    x = np.arange(len(dataset_names))
    width = 0.25

    for wi, (label, color) in enumerate(zip(workload_labels, colors)):
        means = [results[ds][wi]["mean_mops"] for ds in dataset_names if ds in results]
        lo_err = [results[ds][wi]["mean_mops"] - results[ds][wi]["ci_lo"] for ds in dataset_names if ds in results]
        hi_err = [results[ds][wi]["ci_hi"] - results[ds][wi]["mean_mops"] for ds in dataset_names if ds in results]
        ax.bar(x + wi * width, means, width, label=label, color=color, alpha=0.85,
               yerr=[lo_err, hi_err], capsize=4)

    ax.set_xlabel("Dataset", fontsize=12)
    ax.set_ylabel("Throughput (Mops/s)", fontsize=12)
    ax.set_title("HRT-LI Workload Mix Analysis", fontsize=13, fontweight="bold")
    ax.set_xticks(x + width)
    ax.set_xticklabels([ds.replace("_", " ").title() for ds in dataset_names], fontsize=11)
    ax.legend(fontsize=10)
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    path = os.path.join(RESULTS_DIR, "workload_experiment.png")
    plt.savefig(path, bbox_inches="tight")
    plt.close()
    print(f"Workload plot saved: {path}")


def main():
    np.random.seed(RANDOM_SEED)
    random.seed(RANDOM_SEED)

    all_results = {}

    scalability = run_scalability_experiment()
    all_results["scalability"] = scalability
    plot_scalability(scalability)

    workloads = run_workload_experiment()
    all_results["workloads"] = workloads
    plot_workloads(workloads)

    ranges = run_range_experiment()
    all_results["range_queries"] = ranges

    out = os.path.join(RESULTS_DIR, "q1_full_benchmark_results.json")
    with open(out, "w") as f:
        json.dump(all_results, f, indent=2)
    print(f"\nAll results saved: {out}")

    # Print summary table
    print("\n" + "="*70)
    print("SUMMARY TABLE — Key Numbers for Paper")
    print("="*70)
    print(f"{'Dataset':<15} {'N=100K (Mops/s)':<20} {'Range (Mops/s)':<20}")
    print("-"*55)
    for ds in ["synthetic", "filesystem", "url", "dns", "json", "packages"]:
        sc = scalability.get(ds, [])
        rq = ranges.get(ds, {})
        lookup_100k = next((d["mean_mops"] for d in sc if d["N"] == 100_000), None)
        s_lookup = f"{lookup_100k:.3f}" if lookup_100k else "N/A"
        s_range = f"{rq.get('mean_mops', 0):.3f}" if rq else "N/A"
        print(f"{ds:<15} {s_lookup:<20} {s_range:<20}")


if __name__ == "__main__":
    main()
