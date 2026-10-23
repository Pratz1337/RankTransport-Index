import os
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

def main():
    csv_path = "results/latency_v2_v3.csv"
    if not os.path.exists(csv_path):
        print(f"Error: {csv_path} not found.")
        return

    # Load data
    df = pd.read_csv(csv_path)

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, ax = plt.subplots(figsize=(10, 5), dpi=300)

    # Plot v2 and v3 traces
    # Using small alpha and line-width to see density and spikes clearly
    ax.plot(df["step"], df["v2_latency_us"], color="#e056fd", alpha=0.6, linewidth=0.75, label="HRT-LI v2 (Snapshot + Delta + Consolidation)")
    ax.plot(df["step"], df["v3_latency_us"], color="#0984e3", alpha=0.8, linewidth=0.75, label="HRT-LI v3 (Certified Kinetic Segment Tree)")

    # Formatting
    ax.set_yscale("log")
    ax.set_xlabel("Write Operation Step", fontsize=11, fontweight="bold", labelpad=8)
    ax.set_ylabel("Latency (microseconds, log scale)", fontsize=11, fontweight="bold", labelpad=8)
    ax.set_title("Latency Spike Elimination: HRT-LI v2 vs. HRT-LI v3", fontsize=13, fontweight="bold", pad=15)
    
    # Highlight periodic consolidation spikes in v2
    ax.annotate("Periodic Consolidation\nSpikes (~1.5 ms)", 
                xy=(1000, 1400), xytext=(1200, 300),
                arrowprops=dict(facecolor="#d63031", shrink=0.08, width=1.5, headwidth=6),
                fontsize=9, fontweight="bold", color="#d63031")
    
    ax.annotate("Smooth local Kinetic splits\n(No consolidation spikes)", 
                xy=(3500, 20), xytext=(2200, 1.5),
                arrowprops=dict(facecolor="#0984e3", shrink=0.08, width=1.5, headwidth=6),
                fontsize=9, fontweight="bold", color="#0984e3")

    ax.legend(loc="upper right", frameon=True, facecolor="white", framealpha=0.9, edgecolor="none")
    ax.set_ylim(0.05, 5000)

    # Save plot
    save_path = "results/latency_comparison.png"
    plt.savefig(save_path, bbox_inches="tight")
    save_path_q1 = "results_q1/latency_comparison.png"
    os.makedirs(os.path.dirname(save_path_q1), exist_ok=True)
    plt.savefig(save_path_q1, bbox_inches="tight")
    plt.close()
    print(f"Generated latency comparison plot at: {save_path} and {save_path_q1}")

if __name__ == "__main__":
    main()
