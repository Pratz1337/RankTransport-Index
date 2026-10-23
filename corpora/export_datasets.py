import os
from download_publishable_corpora import build_publishable_corpora
from hli.datasets import (
    generate_scalable_tree,
    load_filesystem_paths,
)

def export_dataset(name, paths, initial_ratio=0.8):
    os.makedirs("data", exist_ok=True)
    
    n = len(paths)
    split = int(n * initial_ratio)
    initial_keys = paths[:split]
    insert_keys = paths[split:]
    
    # Write initial keys
    with open(f"data/{name}_initial.txt", "w", encoding="utf-8") as f:
        for k in initial_keys:
            f.write(k + "\n")
            
    # Write insert keys
    with open(f"data/{name}_insert.txt", "w", encoding="utf-8") as f:
        for k in insert_keys:
            f.write(k + "\n")
            
    print(f"Exported {name}: {len(initial_keys)} initial, {len(insert_keys)} insert keys.")

def main():
    # Generate datasets of size 10,000 to keep it lightweight but representative
    export_dataset("synthetic", generate_scalable_tree(10000, max_depth=8, seed=11))
    export_dataset("filesystem", load_filesystem_paths(os.path.dirname(__file__), max_paths=10000))
    publishable = build_publishable_corpora(max_paths=10000)
    for name in ("url", "dns", "json", "package"):
        export_dataset(name, publishable[name])

if __name__ == "__main__":
    main()
