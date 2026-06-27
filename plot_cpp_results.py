import json
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = "results_q1"
JSON_PATH = os.path.join(RESULTS_DIR, "cpp_benchmark_results.json")

def main():
    if not os.path.exists(JSON_PATH):
        print(f"Error: JSON file not found at {JSON_PATH}")
        return

    with open(JSON_PATH, "r") as f:
        data = json.load(f)

    datasets = list(data.keys())
    # Find all unique index names in the results
    index_names = []
    for ds in datasets:
        for r in data[ds]:
            if r["index_name"] not in index_names:
                index_names.append(r["index_name"])

    print("Datasets:", datasets)
    print("Indexes:", index_names)

    # Let's generate a separate comparison plot for each metric across the datasets
    metrics = {
        "load_time_ms": "Bulk Load Time (ms) - Lower is Better",
        "insert_time_ms": "Bulk Insert Time (ms) - Lower is Better",
        "lookup_throughput_mops": "Lookup Throughput (Million ops/sec) - Higher is Better",
        "avg_lookup_latency_ns": "Average Lookup Latency (ns) - Lower is Better"
    }

    # Vibrant modern palette
    colors = ["#2b5c8f", "#d95f02", "#7570b3", "#e7298a", "#66a61e", "#e6ab02", "#a6761d"]

    for metric_name, title in metrics.items():
        plt.figure(figsize=(12, 6), dpi=150)
        
        x = np.arange(len(datasets))
        width = 0.12
        
        for idx, index_name in enumerate(index_names):
            values = []
            for ds in datasets:
                # Find the result for this index in this dataset
                val = 0.0
                for r in data[ds]:
                    if r["index_name"] == index_name:
                        val = r[metric_name]
                        break
                values.append(val)
                
            plt.bar(x + (idx - len(index_names)/2.0 + 0.5) * width, values, width, 
                    label=index_name, color=colors[idx % len(colors)])

        plt.xlabel("Datasets", fontsize=12, fontweight='bold')
        plt.ylabel(title, fontsize=12, fontweight='bold')
        plt.title(f"C++ Competitor Comparison: {title.split(' - ')[0]}", fontsize=14, fontweight='bold', pad=15)
        plt.xticks(x, [ds.capitalize() for ds in datasets], fontsize=11)
        plt.grid(axis='y', linestyle='--', alpha=0.5)
        plt.legend(frameon=True, facecolor='white', edgecolor='none', shadow=True)
        plt.tight_layout()
        
        save_path = os.path.join(RESULTS_DIR, f"cpp_{metric_name}.png")
        plt.savefig(save_path, bbox_inches='tight')
        plt.close()
        print(f"Generated plot: {save_path}")

if __name__ == "__main__":
    main()
