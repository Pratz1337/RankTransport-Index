"""
LEGACY DIAGNOSTIC ONLY.

This file preserves the earlier CD-HLI conformal-warp benchmark for comparison.
It is not the Q1-facing methodology because Mobius/Blaschke coordinate warping
does not provide an exact dynamic rank certificate. Use benchmark_hrtli.py or
benchmark_q1.py for the current publishable experiment.

Former title: Q1-Grade Benchmark Suite for CD-HLI.
Compares CD-HLI against ALEX, PGM, ART, and B+-Tree on real+synthetic datasets
with proper statistical methodology (multiple runs, confidence intervals).
"""
import os, time, json, random
import numpy as np
import torch
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from collections import defaultdict

from hli.tree_generator import generate_hierarchical_data, TreeNode
from hli.poincare_embedding import embed_tree_poincare
from hli.models import HyperbolicMLP, EuclideanMLP, train_model, prepare_datasets, string_to_float
from hli.blaschke import blaschke_product, apply_blaschke_warp
from hli.conformal import apply_conformal_warp
from hli.dynamic_embedding import incremental_poincare_insert, WarpErrorTracker
from hli.datasets import (load_linux_kernel_paths, generate_dns_paths,
                           generate_json_paths, generate_scalable_tree, dataset_stats)
from hli.baselines import GappedArrayIndex, PiecewiseLinearIndex, AdaptiveRadixTree, BPlusTreeIndex

RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results_q1")
os.makedirs(RESULTS_DIR, exist_ok=True)

# ─── Helpers ─────────────────────────────────────────────────────────────────

def paths_to_tree(paths):
    """Convert flat path list into TreeNode dict + sorted list for Poincaré embedding."""
    nodes = {}
    path_to_id = {}
    root = TreeNode(node_id=0, path=paths[0].split('/')[0] or "/root", depth=0)
    nodes[0] = root
    path_to_id[root.path] = 0
    nid = 1
    for p in paths:
        parts = p.strip('/').split('/')
        for d in range(1, len(parts)+1):
            sub = '/' + '/'.join(parts[:d])
            if sub not in path_to_id:
                parent_path = '/' + '/'.join(parts[:d-1]) if d > 1 else root.path
                pid = path_to_id.get(parent_path, 0)
                node = TreeNode(node_id=nid, path=sub, depth=d, parent_id=pid)
                nodes[nid] = node
                path_to_id[sub] = nid
                nodes[pid].children_ids.append(nid)
                nid += 1
    sorted_nodes = sorted(nodes.values(), key=lambda n: n.path)
    for idx, node in enumerate(sorted_nodes):
        node.physical_index = idx
        node.cdf = idx / max(1, len(sorted_nodes)-1)
    return nodes, sorted_nodes

def evaluate_model_errors(model, features, sorted_nodes):
    """Compute prediction errors for a trained model."""
    N = len(sorted_nodes)
    with torch.no_grad():
        preds = model(features).numpy().flatten()
    pred_indices = np.clip(np.round(preds * (N-1)).astype(int), 0, N-1)
    true_indices = np.array([n.physical_index for n in sorted_nodes])
    abs_errors = np.abs(pred_indices - true_indices)
    return {
        "max_error": int(np.max(abs_errors)),
        "mean_error": float(np.mean(abs_errors)),
        "p50_error": float(np.percentile(abs_errors, 50)),
        "p99_error": float(np.percentile(abs_errors, 99)),
        "abs_errors": abs_errors,
    }

# ─── Experiment 1: Static Lookup Benchmark ──────────────────────────────────

def run_static_benchmark(dataset_name, paths, max_keys=10000):
    """Benchmarks static lookup performance across all methods."""
    paths = paths[:max_keys]
    print(f"\n{'='*60}")
    print(f" Static Benchmark: {dataset_name} ({len(paths):,} keys)")
    print(f"{'='*60}")

    # Build tree + Poincaré embedding
    nodes, sorted_nodes = paths_to_tree(paths)
    N = len(sorted_nodes)
    print(f"  Tree built: {N} nodes")
    embed_tree_poincare(nodes, alpha=0.5, spacing=0.85)

    # Train CD-HLI model
    x_euc, x_hyp, y = prepare_datasets(sorted_nodes)
    hli_model = HyperbolicMLP(hidden_dim=64, layers=2)
    train_model(hli_model, x_hyp, y, epochs=300, lr=0.005, verbose=False)
    euc_model = EuclideanMLP(hidden_dim=64, layers=2)
    train_model(euc_model, x_euc, y, epochs=300, lr=0.005, verbose=False)

    hli_errs = evaluate_model_errors(hli_model, x_hyp, sorted_nodes)
    euc_errs = evaluate_model_errors(euc_model, x_euc, sorted_nodes)

    # Baselines
    keys_sorted = [n.path for n in sorted_nodes]
    bpt = BPlusTreeIndex(); bpt.build(keys_sorted)
    alex = GappedArrayIndex(); alex.build(keys_sorted)
    pgm = PiecewiseLinearIndex(epsilon=64); pgm.build(keys_sorted)
    art = AdaptiveRadixTree(); art.build(keys_sorted)

    # Sample lookups
    sample = random.sample(range(N), min(N, 2000))
    results = {}
    for name, idx_obj in [("B+-Tree", bpt), ("ALEX", alex), ("PGM", pgm), ("ART", art)]:
        comps = []
        t0 = time.perf_counter()
        for i in sample:
            _, c = idx_obj.lookup(sorted_nodes[i].path)
            comps.append(c)
        elapsed = (time.perf_counter() - t0) / len(sample) * 1e6
        results[name] = {"avg_comp": np.mean(comps), "p99_comp": np.percentile(comps,99),
                         "latency_us": elapsed, "memory_bytes": idx_obj.memory_bytes()}

    # HLI lookup (model predict + local binary search within epsilon)
    hli_eps = hli_errs["max_error"]
    euc_eps = euc_errs["max_error"]
    for label, model, feats, eps in [("Euclidean LI", euc_model, x_euc, euc_eps),
                                      ("CD-HLI", hli_model, x_hyp, hli_eps)]:
        comps = []
        t0 = time.perf_counter()
        model.eval()
        with torch.no_grad():
            for i in sample:
                pred_cdf = model(feats[i].unsqueeze(0)).item()
                pred_idx = max(0, min(N-1, int(round(pred_cdf*(N-1)))))
                lo, hi = max(0, pred_idx-eps), min(N-1, pred_idx+eps)
                c = 0
                target = sorted_nodes[i].path
                while lo <= hi:
                    mid = (lo+hi)//2; c += 1
                    if sorted_nodes[mid].path == target: break
                    elif sorted_nodes[mid].path < target: lo = mid+1
                    else: hi = mid-1
                comps.append(c)
        elapsed = (time.perf_counter() - t0) / len(sample) * 1e6
        param_bytes = sum(p.numel()*4 for p in model.parameters())
        results[label] = {"avg_comp": np.mean(comps), "p99_comp": np.percentile(comps,99),
                          "latency_us": elapsed, "memory_bytes": param_bytes,
                          "epsilon": eps, "mean_pred_err": euc_errs["mean_error"] if "Euc" in label else hli_errs["mean_error"]}

    # Print table
    print(f"\n  {'Method':<20} {'Avg Comp':>10} {'P99 Comp':>10} {'Latency(μs)':>12} {'Memory':>12}")
    print(f"  {'-'*64}")
    for name, r in results.items():
        mem = f"{r['memory_bytes']:,}B"
        print(f"  {name:<20} {r['avg_comp']:>10.2f} {r['p99_comp']:>10.1f} {r['latency_us']:>12.2f} {mem:>12}")

    return {"dataset": dataset_name, "num_keys": N, "results": results,
            "hli_errors": {k:v for k,v in hli_errs.items() if k != "abs_errors"},
            "euc_errors": {k:v for k,v in euc_errs.items() if k != "abs_errors"}}

# ─── Experiment 2: Dynamic Write Benchmark ──────────────────────────────────

def run_dynamic_benchmark(dataset_name, paths, insert_count=500, max_keys=5000):
    """Benchmarks write adaptation: CD-HLI conformal warp vs baseline re-indexing."""
    paths = paths[:max_keys]
    print(f"\n{'='*60}")
    print(f" Dynamic Write Benchmark: {dataset_name}")
    print(f" Initial: {len(paths)} keys, Inserting: {insert_count} keys")
    print(f"{'='*60}")

    nodes, sorted_nodes = paths_to_tree(paths)
    N_init = len(sorted_nodes)
    embed_tree_poincare(nodes, alpha=0.5, spacing=0.85)

    x_euc, x_hyp, y = prepare_datasets(sorted_nodes)
    hli_model = HyperbolicMLP(hidden_dim=64, layers=2)
    train_model(hli_model, x_hyp, y, epochs=300, lr=0.005, verbose=False)
    init_errs = evaluate_model_errors(hli_model, x_hyp, sorted_nodes)
    tracker = WarpErrorTracker(initial_epsilon=init_errs["max_error"])
    print(f"  Initial ε (HLI): {init_errs['max_error']}")

    # Pick insertion parent
    mid_node = sorted_nodes[N_init // 2]
    new_paths = [f"{mid_node.path}/dyn_{i:04d}" for i in range(insert_count)]

    # ── CD-HLI path: conformal warp, zero retraining ──
    t0 = time.perf_counter()
    updated_sorted, new_nodes = incremental_poincare_insert(
        nodes, sorted_nodes, new_paths, mid_node, alpha=0.5, spacing=0.85)
    insertion_angle = mid_node.theta
    poles, weights = apply_blaschke_warp(updated_sorted,
        [{"angle": insertion_angle, "num_inserted": insert_count, "total_nodes": N_init}], alpha=0.3)
    cdhli_time = (time.perf_counter() - t0) * 1e6

    # Evaluate post-warp error (without retraining)
    x_hyp_new = torch.tensor([[n.x, n.y, n.radius, n.theta, n.hyperbolic_dist]
                               for n in updated_sorted], dtype=torch.float32)
    post_errs = evaluate_model_errors(hli_model, x_hyp_new, updated_sorted)
    tracker.record_warp(poles, np.array(weights), post_errs["max_error"], insert_count, len(updated_sorted))

    # ── Baselines: full rebuild cost ──
    all_keys = [n.path for n in updated_sorted]
    baseline_metrics = {}
    for bname, bclass in [("B+-Tree", BPlusTreeIndex), ("ALEX", GappedArrayIndex),
                           ("PGM", PiecewiseLinearIndex), ("ART", AdaptiveRadixTree)]:
        idx = bclass() if bname != "PGM" else bclass(epsilon=64)
        idx.build([n.path for n in sorted_nodes[:N_init]])
        m = idx.bulk_insert(new_paths)
        baseline_metrics[bname] = m

    print(f"\n  {'Method':<20} {'Time(μs)':>12} {'Retrains':>10} {'Shifts':>10} {'Post-ε':>8}")
    print(f"  {'-'*60}")
    print(f"  {'CD-HLI (Warp)':<20} {cdhli_time:>12.0f} {'0':>10} {'0':>10} {post_errs['max_error']:>8}")
    for bname, m in baseline_metrics.items():
        print(f"  {bname:<20} {m.get('time_us',0):>12.0f} {m.get('retrains',0):>10} {m.get('shifts',0):>10} {'N/A':>8}")

    return {"dataset": dataset_name, "initial_keys": N_init, "inserted": insert_count,
            "cdhli_time_us": cdhli_time, "cdhli_post_epsilon": post_errs["max_error"],
            "initial_epsilon": init_errs["max_error"],
            "warp_summary": tracker.get_summary(),
            "baseline_metrics": {k: {kk: vv for kk, vv in v.items()} for k, v in baseline_metrics.items()}}

# ─── Experiment 3: Warp Accumulation Stress Test ─────────────────────────────

def run_warp_accumulation_test(num_rounds=10, inserts_per_round=200):
    """Tests how error degrades over successive warps without retraining."""
    print(f"\n{'='*60}")
    print(f" Warp Accumulation Test: {num_rounds} rounds × {inserts_per_round} inserts")
    print(f"{'='*60}")

    nodes, sorted_nodes = generate_hierarchical_data(3000, 6, 2, 5)
    embed_tree_poincare(nodes, alpha=0.5, spacing=0.85)
    x_euc, x_hyp, y = prepare_datasets(sorted_nodes)
    model = HyperbolicMLP(hidden_dim=64, layers=2)
    train_model(model, x_hyp, y, epochs=300, lr=0.005, verbose=False)
    init_errs = evaluate_model_errors(model, x_hyp, sorted_nodes)
    tracker = WarpErrorTracker(initial_epsilon=init_errs["max_error"])

    history = [{"round": 0, "total_keys": len(sorted_nodes),
                "empirical_epsilon": init_errs["max_error"],
                "theoretical_epsilon": float(init_errs["max_error"]),
                "mean_error": init_errs["mean_error"]}]

    current_sorted = sorted_nodes
    for rnd in range(1, num_rounds + 1):
        parent = current_sorted[len(current_sorted) // 2]
        new_paths = [f"{parent.path}/r{rnd}_{i:03d}" for i in range(inserts_per_round)]
        current_sorted, _ = incremental_poincare_insert(
            nodes, current_sorted, new_paths, parent, alpha=0.5, spacing=0.85)
        angle = parent.theta
        poles, weights = apply_blaschke_warp(current_sorted,
            [{"angle": angle, "num_inserted": inserts_per_round,
              "total_nodes": len(current_sorted) - inserts_per_round}], alpha=0.3)

        x_new = torch.tensor([[n.x, n.y, n.radius, n.theta, n.hyperbolic_dist]
                               for n in current_sorted], dtype=torch.float32)
        errs = evaluate_model_errors(model, x_new, current_sorted)
        entry = tracker.record_warp(poles, np.array(weights), errs["max_error"],
                                     inserts_per_round, len(current_sorted))
        history.append({"round": rnd, "total_keys": len(current_sorted),
                        "empirical_epsilon": errs["max_error"],
                        "theoretical_epsilon": entry["theoretical_epsilon_bound"],
                        "mean_error": errs["mean_error"]})
        print(f"  Round {rnd}: N={len(current_sorted)}, ε_emp={errs['max_error']}, "
              f"ε_theory={entry['theoretical_epsilon_bound']:.0f}, "
              f"mean_err={errs['mean_error']:.1f}")

    # Plot
    rounds = [h["round"] for h in history]
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5), dpi=150)
    ax1.plot(rounds, [h["empirical_epsilon"] for h in history], 'o-', color='#10b981', lw=2, label='Empirical ε')
    ax1.plot(rounds, [h["theoretical_epsilon"] for h in history], 's--', color='#f43f5e', lw=2, label='Theoretical Bound')
    ax1.set_xlabel("Warp Round"); ax1.set_ylabel("Worst-Case Error (ε)")
    ax1.set_title("Error Bound Accumulation Over Successive Warps", fontweight='bold')
    ax1.legend(); ax1.grid(True, alpha=0.3)

    ax2.plot(rounds, [h["mean_error"] for h in history], 'D-', color='#3b82f6', lw=2)
    ax2.set_xlabel("Warp Round"); ax2.set_ylabel("Mean Prediction Error")
    ax2.set_title("Average Error Degradation", fontweight='bold')
    ax2.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(os.path.join(RESULTS_DIR, "warp_accumulation.png"), bbox_inches='tight')
    plt.close()
    return history

# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    print("="*70)
    print(" LEGACY CD-HLI DIAGNOSTIC BENCHMARK (NOT Q1-FACING)")
    print("="*70)
    random.seed(42); np.random.seed(42); torch.manual_seed(42)

    all_results = {}

    # --- Static benchmarks across multiple datasets ---
    datasets = {
        "Linux Kernel Paths": load_linux_kernel_paths(max_paths=8000),
        "DNS Zone Records": generate_dns_paths(8000),
        "JSON Document Paths": generate_json_paths(8000),
        "Synthetic Tree (8K)": generate_scalable_tree(8000),
    }
    for dname, paths in datasets.items():
        stats = dataset_stats(paths)
        print(f"\n[DATA] {dname}: {stats['num_keys']:,} keys, depth [{stats['min_depth']}-{stats['max_depth']}]")
        r = run_static_benchmark(dname, paths, max_keys=8000)
        all_results[f"static_{dname}"] = r

    # --- Dynamic write benchmarks ---
    for dname, paths in [("Linux Kernel Paths", datasets["Linux Kernel Paths"]),
                          ("DNS Zone Records", datasets["DNS Zone Records"])]:
        r = run_dynamic_benchmark(dname, paths, insert_count=500, max_keys=5000)
        all_results[f"dynamic_{dname}"] = r

    # --- Warp accumulation stress test ---
    history = run_warp_accumulation_test(num_rounds=10, inserts_per_round=200)
    all_results["warp_accumulation"] = history

    # Save results
    def convert(obj):
        if isinstance(obj, (np.integer,)): return int(obj)
        if isinstance(obj, (np.floating,)): return float(obj)
        if isinstance(obj, np.ndarray): return obj.tolist()
        raise TypeError(f"Not serializable: {type(obj)}")

    with open(os.path.join(RESULTS_DIR, "benchmark_results.json"), "w") as f:
        json.dump(all_results, f, indent=2, default=convert)

    print(f"\n{'='*70}")
    print(f" ALL BENCHMARKS COMPLETE — Results saved to {RESULTS_DIR}/")
    print(f"{'='*70}")

if __name__ == "__main__":
    main()
