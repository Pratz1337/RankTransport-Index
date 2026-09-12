"""Q1-facing benchmark for HRT-LI: Hyperbolic Rank-Transport Learned Index."""

from __future__ import annotations

import json
import os
import random
import time

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from hli.baselines import AdaptiveRadixTree, BPlusTreeIndex, GappedArrayIndex, PiecewiseLinearIndex
from hli.datasets import (
    dataset_stats,
    generate_scalable_tree,
    load_filesystem_paths,
)
from hli.poincare_embedding import embed_tree_poincare
from hli.rank_transport import RankTransportIndex
from hli.tree_generator import TreeNode
from download_publishable_corpora import build_publishable_corpora


RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results_q1")


def paths_to_tree(paths: list[str]) -> tuple[dict[int, TreeNode], list[TreeNode]]:
    nodes: dict[int, TreeNode] = {}
    path_to_id: dict[str, int] = {}
    root = TreeNode(node_id=0, path="/root", depth=0)
    nodes[0] = root
    path_to_id[root.path] = 0
    next_id = 1

    for path in sorted(set(paths)):
        clean_parts = [part for part in path.strip("/").split("/") if part]
        parent_id = 0
        parent_path = ""
        for depth, part in enumerate(clean_parts, start=1):
            current_path = f"{parent_path}/{part}" if parent_path else f"/{part}"
            if current_path not in path_to_id:
                node = TreeNode(next_id, current_path, depth, parent_id)
                nodes[next_id] = node
                path_to_id[current_path] = next_id
                nodes[parent_id].children_ids.append(next_id)
                next_id += 1
            parent_id = path_to_id[current_path]
            parent_path = current_path

    sorted_nodes = sorted(nodes.values(), key=lambda node: node.path)
    for idx, node in enumerate(sorted_nodes):
        node.physical_index = idx
        node.cdf = idx / max(1, len(sorted_nodes) - 1)
    return nodes, sorted_nodes


def build_certified_base_model(base_keys: list[str], epsilon: int = 64) -> tuple[dict[str, int], dict[str, float | int]]:
    """Build a PGM-style learned rank model with an explicit worst-case epsilon."""
    model = PiecewiseLinearIndex(epsilon=epsilon)
    model.build(base_keys)
    pred_indices = np.array([model.predict_position(key) for key in base_keys])
    true_indices = np.arange(len(base_keys))
    errors = np.abs(pred_indices - true_indices)
    predicted = {key: int(pred_indices[i]) for i, key in enumerate(base_keys)}
    stats = {
        "epsilon": int(np.max(errors)),
        "mean_error": float(np.mean(errors)),
        "p95_error": float(np.percentile(errors, 95)),
        "p99_error": float(np.percentile(errors, 99)),
        "target_epsilon": epsilon,
        "segments": len(model.segments),
        "model_bytes_estimate": model.memory_bytes(),
    }
    if stats["epsilon"] > epsilon:
        raise ValueError(f"Requested epsilon {epsilon} failed: realized {stats['epsilon']}")
    return predicted, stats


def make_insert_batch(parent_keys: list[str], round_id: int, inserts_per_parent: int) -> list[str]:
    keys = []
    for parent_idx, parent in enumerate(parent_keys):
        clean_parent = parent.rstrip("/")
        for i in range(inserts_per_parent):
            keys.append(f"{clean_parent}/__hrtli_r{round_id:02d}_p{parent_idx:02d}_{i:04d}")
    return keys


def audit_live_state(index: RankTransportIndex, predicted_base: dict[str, int],
                     expected_live: set[str], epsilon: int) -> dict:
    """Check every live rank against input-derived state, never an index snapshot."""
    expected_ranks = {key: rank for rank, key in enumerate(sorted(expected_live))}
    if len(index) != len(expected_live):
        raise ValueError("Live cardinality disagrees with independent oracle")
    stale_error = transported_error = checked_base = 0
    for key, rank in expected_ranks.items():
        if not index.contains(key) or index.lookup(key, predicted_base.get(key, 0)) != rank:
            raise ValueError(f"Lookup disagrees with independent oracle: {key!r}")
        if index.exact_rank(key) != rank:
            raise ValueError(f"Exact rank disagrees with independent oracle: {key!r}")
        if key in predicted_base:
            checked_base += 1
            stale_error = max(stale_error, abs(predicted_base[key] - rank))
            transported_error = max(transported_error,
                                    abs(index.transport_base_prediction(key, predicted_base[key]) - rank))
    for key in predicted_base.keys() - expected_live:
        if index.contains(key) or index.lookup(key, predicted_base[key]) != -1:
            raise ValueError(f"Deleted base key remains visible: {key!r}")
    if transported_error > epsilon:
        raise ValueError(f"Transported error {transported_error} exceeds requested {epsilon}")
    return {"stale_no_transport_max_error": stale_error,
            "transported_max_error": transported_error, "bound_holds": True,
            "inserted_rank_exact": True, "checked_live_ranks": len(expected_live),
            "checked_surviving_base": checked_base,
            "checked_inserted_ranks": len(expected_live) - checked_base}


def run_rank_transport_experiment(dataset_name: str, paths: list[str], initial_keys: int = 2600) -> dict:
    print(f"\n{'=' * 72}")
    print(f"HRT-LI dynamic experiment: {dataset_name}")
    print(f"{'=' * 72}")

    nodes, sorted_nodes = paths_to_tree(paths[:initial_keys])
    embed_tree_poincare(nodes, alpha=0.5, spacing=0.85)
    base_keys = [node.path for node in sorted_nodes]
    predicted_base, base_stats = build_certified_base_model(base_keys, epsilon=64)
    index = RankTransportIndex(base_keys, int(base_stats["target_epsilon"]))
    expected_live = set(base_keys)

    print(
        f"Base learned model: N={len(base_keys):,}, epsilon={base_stats['epsilon']}, "
        f"mean_error={base_stats['mean_error']:.2f}, segments={base_stats['segments']:,}"
    )

    parent_positions = [len(base_keys) // 5, len(base_keys) // 2, (len(base_keys) * 4) // 5]
    parent_keys = [base_keys[pos] for pos in parent_positions]
    history = []

    for round_id in range(1, 7):
        batch = make_insert_batch(parent_keys, round_id, inserts_per_parent=35)
        if len(set(batch)) != len(batch) or expected_live.intersection(batch):
            raise ValueError("Mutation fixture does not contain distinct new keys")
        start = time.perf_counter()
        for key in batch:
            if not index.insert(key):
                raise ValueError(f"Expected insertion failed: {key!r}")
        insert_time_us = (time.perf_counter() - start) * 1e6
        expected_live.update(batch)

        # Delete a few base keys after round 3 to verify signed rank transport.
        deleted = 0
        if round_id >= 3:
            for key in base_keys[round_id : round_id + 10]:
                expected_change = key in expected_live
                if index.delete(key) != expected_change:
                    raise ValueError(f"Deletion outcome disagrees with oracle: {key!r}")
                deleted += int(expected_change)
                expected_live.discard(key)

        audit = audit_live_state(index, predicted_base, expected_live, base_stats["target_epsilon"])
        entry = {
            "round": round_id,
            "current_size": len(index),
            "inserted_total": index.delta.inserted_count,
            "deleted_base_total": index.delta.deleted_count,
            "round_insert_time_us": insert_time_us,
            "deleted_this_round": deleted,
            **audit,
        }
        history.append(entry)
        print(
            f"Round {round_id}: size={entry['current_size']:,}, "
            f"stale_error={entry['stale_no_transport_max_error']}, transported_error={entry['transported_max_error']}, "
            f"bound={entry['bound_holds']}, inserted_exact={entry['inserted_rank_exact']}"
        )

    baseline_insert_keys = make_insert_batch(parent_keys, 99, inserts_per_parent=120)
    baseline_results = benchmark_update_baselines(base_keys, baseline_insert_keys)

    return {
        "dataset": dataset_name,
        "dataset_stats": dataset_stats(paths[:initial_keys]),
        "indexed_base_stats": dataset_stats(base_keys),
        "mutation_source": "generated suffixes; correctness fixture, not natural workload evidence",
        "oracle": "independently maintained input-derived Python set, exhaustive live ranks",
        "base_model": base_stats,
        "history": history,
        "baseline_insert_results": baseline_results,
    }


def benchmark_update_baselines(
    base_keys: list[str], insert_keys: list[str]
) -> dict[str, dict[str, float | int]]:
    # Use the same initial state as each reference, not the six-round mutated state.
    hrt_index = RankTransportIndex(base_keys, 0)
    baselines = {
        "B+-Tree": BPlusTreeIndex(),
        "ALEX-style gapped array": GappedArrayIndex(),
        "PGM-style piecewise": PiecewiseLinearIndex(epsilon=64),
        "ART trie": AdaptiveRadixTree(),
    }
    results: dict[str, dict[str, float | int]] = {}

    start = time.perf_counter()
    for key in insert_keys:
        hrt_index.insert(key)
    hrt_time = (time.perf_counter() - start) * 1e6
    results["HRT-LI delta transport"] = {
        "time_us": hrt_time,
        "shifts": 0,
        "retrains": 0,
        "memory_bytes": hrt_index.mutation_count * 72,
    }

    for name, baseline in baselines.items():
        baseline.build(base_keys)
        metrics = baseline.bulk_insert(insert_keys)
        results[name] = {
            "time_us": float(metrics.get("time_us", 0.0)),
            "shifts": int(metrics.get("shifts", 0)),
            "retrains": int(metrics.get("retrains", 0)),
            "memory_bytes": int(baseline.memory_bytes()),
        }
    return results


def save_plots(results: dict) -> None:
    os.makedirs(RESULTS_DIR, exist_ok=True)

    for name, result in results.items():
        history = result["history"]
        rounds = [row["round"] for row in history]
        stale = [row["stale_no_transport_max_error"] for row in history]
        transported = [row["transported_max_error"] for row in history]
        epsilon = [result["base_model"]["epsilon"] for _ in rounds]

        plt.figure(figsize=(8.5, 4.8), dpi=150)
        plt.plot(rounds, stale, "o-", color="#d9480f", label="Static model after writes")
        plt.plot(rounds, transported, "s-", color="#087f5b", label="HRT-LI transported model")
        plt.plot(rounds, epsilon, "--", color="#364fc7", label="Initial epsilon certificate")
        plt.xlabel("Write round")
        plt.ylabel("Max rank error on live base keys")
        plt.title(f"Dynamic write-drift correction: {name}")
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        safe_name = name.lower().replace(" ", "_").replace("/", "_")
        plt.savefig(os.path.join(RESULTS_DIR, f"{safe_name}_rank_error.png"), bbox_inches="tight")
        plt.close()

    first = next(iter(results.values()))
    baseline = first["baseline_insert_results"]
    labels = list(baseline.keys())
    times = [baseline[label]["time_us"] for label in labels]
    plt.figure(figsize=(9.5, 4.8), dpi=150)
    bars = plt.bar(labels, times, color=["#087f5b", "#495057", "#5c7cfa", "#ae3ec9", "#0b7285"])
    plt.ylabel("Bulk insert time (microseconds)")
    plt.title("Write adaptation cost without neural retraining")
    plt.xticks(rotation=20, ha="right")
    plt.grid(axis="y", alpha=0.25)
    for bar, value in zip(bars, times):
        plt.text(bar.get_x() + bar.get_width() / 2, bar.get_height(), f"{value:.0f}", ha="center", va="bottom", fontsize=8)
    plt.tight_layout()
    plt.savefig(os.path.join(RESULTS_DIR, "update_cost_comparison.png"), bbox_inches="tight")
    plt.close()


def main() -> None:
    random.seed(42)
    np.random.seed(42)
    os.makedirs(RESULTS_DIR, exist_ok=True)
    publishable = build_publishable_corpora(max_paths=3200)

    datasets = {
        "Synthetic hierarchy": generate_scalable_tree(3200, max_depth=8, seed=11),
        "Filesystem paths": load_filesystem_paths(os.path.dirname(__file__), max_paths=3200),
        "URL paths": publishable["url"],
        "DNS hierarchy": publishable["dns"],
        "JSON paths": publishable["json"],
        "Package paths": publishable["package"],
    }

    results = {
        name: run_rank_transport_experiment(name, paths, initial_keys=2600)
        for name, paths in datasets.items()
    }
    save_plots(results)

    output_path = os.path.join(RESULTS_DIR, "hrtli_benchmark_results.json")
    with open(output_path, "w", encoding="utf-8") as handle:
        json.dump(results, handle, indent=2)

    print(f"\nSaved HRT-LI benchmark artifacts to {RESULTS_DIR}")
    print(f"JSON: {output_path}")


if __name__ == "__main__":
    main()
