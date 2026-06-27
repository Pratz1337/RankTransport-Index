import os
import time
import torch
import numpy as np
import matplotlib.pyplot as plt
# Tabulate is dynamically imported inside format_table if available

from hli.tree_generator import generate_hierarchical_data
from hli.poincare_embedding import embed_tree_poincare
from hli.models import prepare_datasets, train_model, EuclideanMLP, HyperbolicMLP
from hli.search import exact_lookup_hli, exact_lookup_baseline, binary_search_standard, subtree_range_query

def format_table(headers, data):
    """
    Fallback table formatter if tabulate is not installed.
    """
    try:
        from tabulate import tabulate
        return tabulate(data, headers=headers, tablefmt="grid")
    except ImportError:
        # Simple text table fallback
        col_widths = [len(h) for h in headers]
        for row in data:
            for i, val in enumerate(row):
                col_widths[i] = max(col_widths[i], len(str(val)))
        
        fmt = " | ".join([f"{{:<{w}}}" for w in col_widths])
        sep = "-+-".join(["-" * w for w in col_widths])
        
        lines = [fmt.format(*headers), sep]
        for row in data:
            lines.append(fmt.format(*[str(val) for val in row]))
        return "\n".join(lines)

def run_experiment():
    print("=" * 70)
    print(" HLI: HYPERBOLIC LEARNED INDEX FOR HIERARCHICAL DATA - BENCHMARK SUITE")
    print("=" * 70)
    
    # 1. Generate Hierarchical Dataset
    # We generate a tree of 5,000 nodes, which is a perfect representative size.
    # Branching factor between 2 and 5, depth up to 6.
    NUM_NODES = 5000
    MAX_DEPTH = 8
    MIN_BRANCH = 2
    MAX_BRANCH = 5
    
    print(f"\n[Step 1] Generating synthetic hierarchy...")
    print(f"  Target nodes: {NUM_NODES} | Max depth: {MAX_DEPTH}")
    nodes, sorted_nodes = generate_hierarchical_data(
        num_nodes=NUM_NODES, 
        max_depth=MAX_DEPTH, 
        min_branching=MIN_BRANCH, 
        max_branching=MAX_BRANCH
    )
    N = len(sorted_nodes)
    print(f"  Successfully generated tree with {N} nodes.")
    
    # 2. Embed Tree in Poincaré Disk
    print(f"\n[Step 2] Embedding tree in 2D Poincaré disk via recursive angular-partitioning...")
    # Alpha controls radial push, spacing controls sibling wedge buffer
    embed_tree_poincare(nodes, alpha=0.5, spacing=0.85)
    print("  Poincaré embedding completed (dimension = 2).")
    
    # 3. Prepare Datasets for Neural Networks
    print(f"\n[Step 3] Preparing PyTorch tensors...")
    x_euclid, x_hyper, y_target = prepare_datasets(sorted_nodes)
    print(f"  Euclidean features shape: {x_euclid.shape}")
    print(f"  Hyperbolic features shape: {x_hyper.shape}")
    print(f"  Target CDF shape: {y_target.shape}")
    
    # 4. Instantiate and Train Models
    print(f"\n[Step 4] Training Euclidean Learned Index Baseline (1D String-to-Float MLP)...")
    euclid_model = EuclideanMLP(hidden_dim=32, layers=2)
    # Print parameter count
    euclid_params = sum(p.numel() for p in euclid_model.parameters())
    print(f"  Euclidean Model Size: {euclid_params} parameters (~{euclid_params * 4} bytes)")
    train_model(euclid_model, x_euclid, y_target, epochs=400, lr=0.005, verbose=True)
    
    print(f"\n[Step 4b] Training Hyperbolic Learned Index (HLI) Model (2D Poincaré + Inductive Biases)...")
    hli_model = HyperbolicMLP(hidden_dim=32, layers=2)
    hli_params = sum(p.numel() for p in hli_model.parameters())
    print(f"  HLI Model Size: {hli_params} parameters (~{hli_params * 4} bytes)")
    train_model(hli_model, x_hyper, y_target, epochs=400, lr=0.005, verbose=True)
    
    # 5. Compute Mathematical Error Bounds (epsilon)
    # The worst-case prediction error determines the local binary search boundaries.
    print(f"\n[Step 5] Evaluating training prediction errors and determining local search bounds...")
    
    # Euclidean errors
    with torch.no_grad():
        euclid_preds = euclid_model(x_euclid).numpy()
    euclid_pred_indices = np.round(euclid_preds * (N - 1)).astype(int)
    euclid_pred_indices = np.clip(euclid_pred_indices, 0, N - 1).flatten()
    euclid_true_indices = np.array([node.physical_index for node in sorted_nodes])
    euclid_abs_errors = np.abs(euclid_pred_indices - euclid_true_indices)
    euclid_epsilon = int(np.max(euclid_abs_errors))
    
    # HLI errors
    with torch.no_grad():
        hli_preds = hli_model(x_hyper).numpy()
    hli_pred_indices = np.round(hli_preds * (N - 1)).astype(int)
    hli_pred_indices = np.clip(hli_pred_indices, 0, N - 1).flatten()
    hli_abs_errors = np.abs(hli_pred_indices - euclid_true_indices)
    hli_epsilon = int(np.max(hli_abs_errors))
    
    print(f"  Euclidean Baseline Worst-Case Error (epsilon): {euclid_epsilon} nodes")
    print(f"  HLI Worst-Case Error (epsilon): {hli_epsilon} nodes")
    print(f"  HLI reduces the local search neighborhood by {((euclid_epsilon - hli_epsilon) / max(1, euclid_epsilon)) * 100:.1f}%!")
    
    # 6. Benchmark Exact Lookups
    print(f"\n[Step 6] Running Exact Key Lookup Benchmark over all {N} keys...")
    
    # Standard Binary Search (Perfect B-Tree representation)
    bin_comparisons = []
    start_time = time.perf_counter()
    for node in sorted_nodes:
        _, comp = binary_search_standard(node, sorted_nodes)
        bin_comparisons.append(comp)
    bin_latency = (time.perf_counter() - start_time) / N * 1e6  # in microseconds
    
    # Euclidean Baseline Lookup
    euclid_comparisons = []
    start_time = time.perf_counter()
    for idx, node in enumerate(sorted_nodes):
        _, comp, _ = exact_lookup_baseline(euclid_model, node, sorted_nodes, x_euclid[idx], euclid_epsilon)
        euclid_comparisons.append(comp)
    euclid_latency = (time.perf_counter() - start_time) / N * 1e6  # in microseconds
    
    # HLI Lookup
    hli_comparisons = []
    start_time = time.perf_counter()
    for idx, node in enumerate(sorted_nodes):
        _, comp, _ = exact_lookup_hli(hli_model, node, sorted_nodes, x_hyper[idx], hli_epsilon)
        hli_comparisons.append(comp)
    hli_latency = (time.perf_counter() - start_time) / N * 1e6  # in microseconds
    
    # 7. Compile Results and Print Table
    headers = ["Metric", "Binary Search (B-Tree)", "Euclidean Learned Index", "HLI (Proposed)"]
    metrics_data = [
        ["Avg Key Comparisons", f"{np.mean(bin_comparisons):.2f}", f"{np.mean(euclid_comparisons):.2f}", f"{np.mean(hli_comparisons):.2f}"],
        ["99% Key Comparisons", f"{np.percentile(bin_comparisons, 99):.2f}", f"{np.percentile(euclid_comparisons, 99):.2f}", f"{np.percentile(hli_comparisons, 99):.2f}"],
        ["Max Key Comparisons", f"{np.max(bin_comparisons)}", f"{np.max(euclid_comparisons)}", f"{np.max(hli_comparisons)}"],
        ["Index Memory Footprint", "O(N * key_len) (Large)", f"{euclid_params * 4} bytes (Tiny)", f"{hli_params * 4} bytes (Tiny)"],
        ["Avg Lookup Latency (us)", f"{bin_latency:.3f}", f"{euclid_latency:.3f}", f"{hli_latency:.3f}"]
    ]
    
    print("\n" + "=" * 70)
    print(" BENCHMARK PERFORMANCE COMPARISON")
    print("=" * 70)
    print(format_table(headers, metrics_data))
    print("=" * 70)
    
    # 8. Benchmark Subtree Range Queries
    print(f"\n[Step 7] Evaluating Subtree Range Query Performance...")
    # Select 50 random nodes with depth in [1, 3] to query subtrees
    eligible_nodes = [node for node in sorted_nodes if 1 <= node.depth <= 3 and len(node.children_ids) > 0]
    num_queries = min(len(eligible_nodes), 50)
    query_nodes = np.random.choice(eligible_nodes, num_queries, replace=False)
    
    standard_range_comps = []
    hli_range_comps = []
    
    for q_node in query_nodes:
        # Standard range query finds the start node via standard binary search, 
        # then binary searches for the prefix boundaries
        _, start_comp = binary_search_standard(q_node, sorted_nodes)
        
        # Binary search for end of prefix standardly
        low = q_node.physical_index
        high = N - 1
        end_idx = low
        prefix = q_node.path + "/"
        pref_comp = 0
        while low <= high:
            mid = (low + high) // 2
            pref_comp += 1
            if sorted_nodes[mid].path == q_node.path or sorted_nodes[mid].path.startswith(prefix):
                end_idx = mid
                low = mid + 1
            else:
                high = mid - 1
        standard_range_comps.append(start_comp + pref_comp)
        
        # HLI range query uses HLI for exact lookup and the same prefix boundary search
        _, hli_rc = subtree_range_query(q_node, sorted_nodes, exact_lookup_hli, hli_model, x_hyper[q_node.physical_index], hli_epsilon)
        hli_range_comps.append(hli_rc)
        
    print(f"  Average key comparisons for Subtree Retrieval:")
    print(f"    Standard B-Tree Range Search: {np.mean(standard_range_comps):.2f}")
    print(f"    HLI Range Search: {np.mean(hli_range_comps):.2f}")
    print(f"    HLI reduces range query overhead by {((np.mean(standard_range_comps) - np.mean(hli_range_comps)) / np.mean(standard_range_comps)) * 100:.1f}%!")
    
    # 9. Visualization & Plotting
    print(f"\n[Step 8] Creating beautiful visualizations...")
    os.makedirs("results", exist_ok=True)
    
    # Plot 1: Poincaré Disk Layout
    fig, ax = plt.subplots(figsize=(8, 8), dpi=150)
    # Draw unit circle boundary
    circle = plt.Circle((0, 0), 1.0, color='gray', fill=False, linestyle='--', linewidth=1.5)
    ax.add_patch(circle)
    
    # Plot connections (edges)
    for node in sorted_nodes:
        if node.parent_id is not None:
            parent = nodes[node.parent_id]
            ax.plot([node.x, parent.x], [node.y, parent.y], color='#e2e8f0', alpha=0.5, linewidth=0.8, zorder=1)
            
    # Scatter plot nodes, color-coded by their sorted database index
    x_coords = [n.x for n in sorted_nodes]
    y_coords = [n.y for n in sorted_nodes]
    indices = [n.physical_index for n in sorted_nodes]
    
    scatter = ax.scatter(x_coords, y_coords, c=indices, cmap='viridis', s=12, zorder=2, alpha=0.85)
    cbar = plt.colorbar(scatter, ax=ax, label="Sorted Database Array Index (CDF Order)", pad=0.03)
    cbar.ax.tick_params(labelsize=9)
    
    ax.set_xlim(-1.05, 1.05)
    ax.set_ylim(-1.05, 1.05)
    ax.set_aspect('equal')
    ax.axis('off')
    ax.set_title("Deterministic Poincaré Disk Embedding of the Database Hierarchy", fontsize=11, fontweight='bold', pad=15)
    plt.tight_layout()
    plot_path_1 = os.path.abspath("results/poincare_embedding.png")
    plt.savefig(plot_path_1, bbox_inches='tight')
    plt.close()
    
    # Plot 2: Prediction Errors comparison
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5), dpi=150)
    
    # Subplot 1: Predictions vs Ground Truth
    ax1.plot(euclid_true_indices, euclid_true_indices, color='gray', linestyle='--', label='Perfect Fit')
    ax1.scatter(euclid_true_indices, hli_pred_indices, color='#3b82f6', s=2, alpha=0.3, label='HLI (Ours)')
    ax1.scatter(euclid_true_indices, euclid_pred_indices, color='#f43f5e', s=2, alpha=0.15, label='Euclidean Baseline')
    ax1.set_xlabel("Ground Truth sorted index")
    ax1.set_ylabel("Predicted sorted index")
    ax1.set_title("CDF Prediction Accuracy", fontsize=11, fontweight='bold')
    ax1.legend()
    ax1.grid(True, linestyle=':', alpha=0.6)
    
    # Subplot 2: Cumulative Error Distributions
    # Sort the absolute errors to compute empirical CDF of errors
    hli_err_sorted = np.sort(hli_abs_errors)
    euclid_err_sorted = np.sort(euclid_abs_errors)
    p = np.arange(1, N + 1) / N
    
    ax2.plot(hli_err_sorted, p, color='#10b981', linewidth=2, label='HLI (Proposed)')
    ax2.plot(euclid_err_sorted, p, color='#f43f5e', linewidth=2, label='Euclidean Baseline')
    ax2.set_xscale('log')
    ax2.set_xlabel("Absolute Prediction Error (Log scale)")
    ax2.set_ylabel("Percentage of nodes with error <= X")
    ax2.set_title("Empirical CDF of Prediction Error (Tighter is Better)", fontsize=11, fontweight='bold')
    ax2.legend()
    ax2.grid(True, which="both", linestyle=':', alpha=0.6)
    
    plt.tight_layout()
    plot_path_2 = os.path.abspath("results/prediction_errors.png")
    plt.savefig(plot_path_2, bbox_inches='tight')
    plt.close()
    
    # Plot 3: Comparisons distribution
    fig, ax = plt.subplots(figsize=(8, 4.5), dpi=150)
    bins = np.arange(0, max(max(euclid_comparisons), max(hli_comparisons)) + 2) - 0.5
    ax.hist(hli_comparisons, bins=bins, alpha=0.7, color='#10b981', label='HLI (Ours)', edgecolor='black', linewidth=0.5)
    ax.hist(euclid_comparisons, bins=bins, alpha=0.5, color='#f43f5e', label='Euclidean Baseline', edgecolor='black', linewidth=0.5)
    ax.axvline(np.mean(bin_comparisons), color='#6b7280', linestyle='--', linewidth=1.5, label='B-Tree Average')
    ax.set_xlabel("Key Comparisons per Exact Lookup")
    ax.set_ylabel("Frequency (Count)")
    ax.set_title("Distribution of Search Overhead (Fewer is Better)", fontsize=11, fontweight='bold')
    ax.legend()
    ax.grid(True, linestyle=':', alpha=0.6)
    plt.tight_layout()
    plot_path_3 = os.path.abspath("results/search_comparisons.png")
    plt.savefig(plot_path_3, bbox_inches='tight')
    plt.close()
    
    print("\n[Step 9] Beautiful figures saved successfully:")
    print(f"  1. Poincaré Disk Layout: {plot_path_1}")
    print(f"  2. Prediction Error Analysis: {plot_path_2}")
    print(f"  3. Search Comparison Histogram: {plot_path_3}")
    print("\nBENCHMARK RUN SUCCESSFULLY COMPLETED!")
    print("=" * 70)

if __name__ == "__main__":
    run_experiment()
