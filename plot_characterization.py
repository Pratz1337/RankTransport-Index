import json
import os
import matplotlib.pyplot as plt
import numpy as np

# Set style
plt.style.use('seaborn-v0_8-whitegrid' if 'seaborn-v0_8-whitegrid' in plt.style.available else 'default')

def plot_concurrent_timeline():
    json_path = 'results_q1/concurrent_timeline.json'
    if not os.path.exists(json_path):
        print(f"Skipping {json_path} (not found)")
        return
        
    with open(json_path, 'r') as f:
        data = json.load(f)
        
    timeline = data['timeline']
    times_s = [t['time_ms'] / 1000.0 for t in timeline]
    # Convert ns to us for readability
    latencies_us = [t['p99_9_latency_ns'] / 1000.0 for t in timeline]
    
    plt.figure(figsize=(10, 5))
    plt.plot(times_s, latencies_us, color='#e06666', linewidth=2, marker='o', markersize=3, label='p99.9 Read Latency')
    plt.xlabel('Time (seconds)', fontsize=12)
    plt.ylabel('p99.9 Latency (microseconds)', fontsize=12)
    plt.title('p99.9 Read Latency Timeline under Concurrent Consolidation (Rebuilds)', fontsize=14, fontweight='bold')
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.tight_layout()
    plt.savefig('results_q1/concurrent_timeline.png', dpi=300)
    plt.close()
    print("Generated concurrent_timeline.png")

def plot_latency_vs_mn():
    json_path = 'results_q1/latency_vs_mn.json'
    if not os.path.exists(json_path):
        print(f"Skipping {json_path} (not found)")
        return
        
    with open(json_path, 'r') as f:
        data = json.load(f)
        
    ratios = data['ratios']
    v2_point = data.get('v2_point_latencies_ns', data.get('point_latencies_ns'))
    v2_rank = data.get('v2_rank_latencies_ns', data.get('rank_latencies_ns'))
    v3_point = data['v3_point_latencies_ns']
    v3_rank = data['v3_rank_latencies_ns']
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5.5))
    
    # Left: Point latency comparison
    ax1.plot(ratios, v2_point, color='#3d85c6', linewidth=2.5, marker='s', markersize=6, label='HRT-LI v2 Point')
    ax1.plot(ratios, v3_point, color='#e06666', linewidth=2.5, marker='o', markersize=6, label='HRT-LI v3 Point')
    ax1.set_xlabel('Mutation Ratio (m/n)', fontsize=12)
    ax1.set_ylabel('Latency (ns)', fontsize=12)
    ax1.set_title('Point Lookup Latency', fontsize=13, fontweight='bold')
    ax1.grid(True, linestyle='--', alpha=0.6)
    ax1.legend(frameon=True)
    
    # Right: Rank latency comparison
    ax2.plot(ratios, v2_rank, color='#3d85c6', linewidth=2.5, marker='s', markersize=6, label='HRT-LI v2 Rank')
    ax2.plot(ratios, v3_rank, color='#e06666', linewidth=2.5, marker='o', markersize=6, label='HRT-LI v3 Rank')
    ax2.set_xlabel('Mutation Ratio (m/n)', fontsize=12)
    ax2.set_ylabel('Latency (ns)', fontsize=12)
    ax2.set_title('Rank Lookup Latency', fontsize=13, fontweight='bold')
    ax2.grid(True, linestyle='--', alpha=0.6)
    ax2.legend(frameon=True)
    
    fig.suptitle('Lookup Latency vs Mutation Ratio (m/n) for V2 and V3', fontsize=15, fontweight='bold')
    plt.tight_layout()
    plt.savefig('results_q1/latency_vs_mn.png', dpi=300)
    plt.close()
    print("Generated latency_vs_mn.png")


def plot_memory_breakdown():
    json_path = 'results_q1/memory_breakdown.json'
    if not os.path.exists(json_path):
        print(f"Skipping {json_path} (not found)")
        return
        
    with open(json_path, 'r') as f:
        data = json.load(f)
        
    datasets = list(data['datasets'].keys())
    base_array = []
    model_segments = []
    hpsfc_table = []
    delta_nodes = []
    
    for ds in datasets:
        ds_data = data['datasets'][ds]
        base_array.append(ds_data['base_array'])
        model_segments.append(ds_data['model_segments'])
        hpsfc_table.append(ds_data['hpsfc_table'])
        delta_nodes.append(ds_data['delta_nodes'])
        
    x = np.arange(len(datasets))
    width = 0.55
    
    plt.figure(figsize=(12, 6))
    
    # Create stacked bar chart
    b1 = plt.bar(x, base_array, width, label='Base Array Keys', color='#4a86e8')
    b2 = plt.bar(x, model_segments, width, bottom=base_array, label='Model Segments', color='#ffd966')
    
    bottom_hpsfc = [b + m for b, m in zip(base_array, model_segments)]
    b3 = plt.bar(x, hpsfc_table, width, bottom=bottom_hpsfc, label='HP-SFC Table', color='#93c47d')
    
    bottom_delta = [bh + h for bh, h in zip(bottom_hpsfc, hpsfc_table)]
    b4 = plt.bar(x, delta_nodes, width, bottom=bottom_delta, label='Delta Nodes', color='#e06666')
    
    plt.xlabel('Datasets', fontsize=12)
    plt.ylabel('Bytes per Key', fontsize=12)
    plt.title('HRT-LI Component Memory Breakdown (Bytes per Key)', fontsize=14, fontweight='bold')
    plt.xticks(x, datasets, fontsize=10, rotation=15)
    plt.legend(frameon=True, facecolor='white', framealpha=0.9)
    plt.grid(True, axis='y', linestyle='--', alpha=0.6)
    plt.tight_layout()
    plt.savefig('results_q1/memory_breakdown.png', dpi=300)
    plt.close()
    print("Generated memory_breakdown.png")

def plot_model_evolution():
    json_path = 'results_q1/model_evolution.json'
    if not os.path.exists(json_path):
        print(f"Skipping {json_path} (not found)")
        return
        
    with open(json_path, 'r') as f:
        data = json.load(f)
        
    generations = data['generations']
    num_segments = data['num_segments']
    max_errors = data['max_errors']
    
    fig, ax1 = plt.subplots(figsize=(10, 5))
    
    color = '#674ea7'
    ax1.set_xlabel('Generation of Out-of-Distribution Inserts', fontsize=12)
    ax1.set_ylabel('Number of Model Segments', color=color, fontsize=12)
    line1 = ax1.plot(generations, num_segments, color=color, linewidth=2, marker='o', label='Segments count')
    ax1.tick_params(axis='y', labelcolor=color)
    ax1.grid(True, linestyle='--', alpha=0.4)
    
    ax2 = ax1.twinx()
    color = '#e06666'
    ax2.set_ylabel('Maximum Prediction Error (Keys)', color=color, fontsize=12)
    line2 = ax2.plot(generations, max_errors, color=color, linewidth=2, marker='^', linestyle='--', label='Max Error')
    ax2.tick_params(axis='y', labelcolor=color)
    
    # Legend
    lines = line1 + line2
    labels = [l.get_label() for l in lines]
    ax1.legend(lines, labels, loc='upper left')
    
    plt.title('Model Evolution Across Out-of-Distribution Insert Generations', fontsize=14, fontweight='bold')
    plt.tight_layout()
    plt.savefig('results_q1/model_evolution.png', dpi=300)
    plt.close()
    print("Generated model_evolution.png")

if __name__ == "__main__":
    plot_concurrent_timeline()
    plot_latency_vs_mn()
    plot_memory_breakdown()
    plot_model_evolution()
