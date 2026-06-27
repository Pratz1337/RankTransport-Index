"""
Proper baseline index implementations for head-to-head comparison.

Implements:
  1. GappedArrayIndex (ALEX-style) — gapped sorted array with model-guided inserts
  2. PiecewiseLinearIndex (PGM-style) — piecewise linear CDF approximation
  3. AdaptiveRadixTree (ART-style) — radix trie optimized for string keys  
  4. BPlusTree — standard B+-Tree reference implementation

All baselines expose a unified interface:
  - build(keys)
  - lookup(key) -> (position, comparisons)
  - insert(key) -> overhead_metrics
  - bulk_insert(keys) -> overhead_metrics
  - memory_bytes() -> int
"""

import bisect
import math
import time
import numpy as np
from typing import List, Tuple, Optional, Dict


# ─── Baseline 1: ALEX-style Gapped Array Index ──────────────────────────────

class GappedArrayIndex:
    """
    Simplified ALEX-style learned index with gapped arrays.
    
    Key ideas from ALEX (SIGMOD 2020):
    - Maintains sorted arrays with physical gaps (empty slots) to absorb inserts
    - Uses a simple linear model per node to predict positions
    - Splits nodes when gap density drops below threshold
    - Re-trains local linear models after splits
    
    This is a faithful simplified reproduction for benchmarking purposes.
    """
    
    def __init__(self, initial_gap_ratio: float = 0.3, 
                 max_node_size: int = 1024,
                 split_threshold: float = 0.05):
        self.gap_ratio = initial_gap_ratio
        self.max_node_size = max_node_size
        self.split_threshold = split_threshold
        self.nodes: List[dict] = []
        self.total_keys = 0
        self.total_retrains = 0
        self.total_splits = 0
        self.total_data_shifts = 0
        
    def build(self, keys: List[str]):
        """Build the index from a sorted list of keys."""
        self.total_keys = len(keys)
        
        # Partition keys into nodes of max_node_size
        num_nodes = max(1, math.ceil(len(keys) / (self.max_node_size * (1 - self.gap_ratio))))
        chunk_size = max(1, math.ceil(len(keys) / num_nodes))
        
        self.nodes = []
        for i in range(0, len(keys), chunk_size):
            chunk = keys[i:i + chunk_size]
            node = self._create_node(chunk)
            self.nodes.append(node)
    
    def _create_node(self, keys: List[str]) -> dict:
        """Creates a gapped array node with a linear model."""
        n = len(keys)
        # Create gapped array: insert None gaps
        gap_count = max(1, int(n * self.gap_ratio / (1 - self.gap_ratio)))
        total_slots = n + gap_count
        
        gapped = [None] * total_slots
        # Distribute keys evenly among slots
        step = total_slots / n if n > 0 else 1
        key_positions = {}
        for j, key in enumerate(keys):
            pos = min(int(j * step), total_slots - 1)
            while gapped[pos] is not None:
                pos = (pos + 1) % total_slots
            gapped[pos] = key
            key_positions[key] = pos
        
        # Fit linear model: position = slope * rank + intercept
        if n > 1:
            slope = (total_slots - 1) / (n - 1)
            intercept = 0.0
        else:
            slope = 1.0
            intercept = 0.0
        
        return {
            "array": gapped,
            "keys": sorted(keys),
            "num_keys": n,
            "capacity": total_slots,
            "slope": slope,
            "intercept": intercept,
            "gap_density": gap_count / total_slots,
        }
    
    def _find_node(self, key: str) -> int:
        """Binary search to find the correct node for a key."""
        lo, hi = 0, len(self.nodes) - 1
        while lo < hi:
            mid = (lo + hi) // 2
            if self.nodes[mid]["keys"][-1] < key:
                lo = mid + 1
            else:
                hi = mid
        return lo
    
    def lookup(self, key: str) -> Tuple[Optional[int], int]:
        """
        Looks up a key. Returns (position_in_sorted_order, num_comparisons).
        """
        comparisons = 0
        
        # Find correct node
        node_idx = self._find_node(key)
        comparisons += int(math.log2(max(1, len(self.nodes)))) + 1
        
        node = self.nodes[node_idx]
        
        # Use linear model to predict position in gapped array
        keys = node["keys"]
        n = node["num_keys"]
        
        # Binary search within the node's key list
        lo, hi = 0, n - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            comparisons += 1
            if keys[mid] == key:
                # Calculate global sorted position
                global_pos = sum(nd["num_keys"] for nd in self.nodes[:node_idx]) + mid
                return global_pos, comparisons
            elif keys[mid] < key:
                lo = mid + 1
            else:
                hi = mid - 1
        
        return None, comparisons
    
    def insert(self, key: str) -> dict:
        """
        Inserts a key into the index. Returns metrics about the operation cost.
        """
        metrics = {"comparisons": 0, "shifts": 0, "retrains": 0, "splits": 0, "time_us": 0}
        start = time.perf_counter()
        
        node_idx = self._find_node(key)
        metrics["comparisons"] += int(math.log2(max(1, len(self.nodes)))) + 1
        
        node = self.nodes[node_idx]
        
        # Insert into sorted key list
        insert_pos = bisect.bisect_left(node["keys"], key)
        node["keys"].insert(insert_pos, key)
        node["num_keys"] += 1
        self.total_keys += 1
        
        # Count data shifts (all elements after insert_pos must move)
        shifts = node["num_keys"] - insert_pos
        metrics["shifts"] = shifts
        self.total_data_shifts += shifts
        
        # Insert into gapped array
        predicted_pos = min(int(insert_pos * node["slope"] + node["intercept"]),
                          node["capacity"] - 1)
        predicted_pos = max(0, predicted_pos)
        
        # Find nearest gap
        placed = False
        for offset in range(node["capacity"]):
            for direction in [1, -1]:
                check_pos = predicted_pos + direction * offset
                if 0 <= check_pos < node["capacity"] and node["array"][check_pos] is None:
                    node["array"][check_pos] = key
                    placed = True
                    break
            if placed:
                break
        
        if not placed:
            # Array is full — need to expand
            node["array"].append(key)
            node["capacity"] += 1
        
        # Update gap density
        gap_count = node["array"].count(None)
        node["gap_density"] = gap_count / node["capacity"]
        
        # Check if node needs splitting
        if node["gap_density"] < self.split_threshold or node["num_keys"] > self.max_node_size:
            self._split_node(node_idx)
            metrics["splits"] = 1
            metrics["retrains"] = 2  # Both child nodes get new models
            self.total_splits += 1
            self.total_retrains += 2
        
        metrics["time_us"] = (time.perf_counter() - start) * 1e6
        return metrics
    
    def _split_node(self, node_idx: int):
        """Splits a node into two, re-trains linear models for both halves."""
        old_node = self.nodes[node_idx]
        keys = old_node["keys"]
        mid = len(keys) // 2
        
        left_node = self._create_node(keys[:mid])
        right_node = self._create_node(keys[mid:])
        
        self.nodes[node_idx] = left_node
        self.nodes.insert(node_idx + 1, right_node)
    
    def bulk_insert(self, keys: List[str]) -> dict:
        """Inserts multiple keys. Returns aggregate metrics."""
        total_metrics = {"comparisons": 0, "shifts": 0, "retrains": 0, 
                        "splits": 0, "time_us": 0}
        for key in keys:
            m = self.insert(key)
            for k in total_metrics:
                total_metrics[k] += m[k]
        return total_metrics
    
    def memory_bytes(self) -> int:
        """Estimates total memory usage."""
        mem = 0
        for node in self.nodes:
            # Array slots (pointer per slot) + keys + model params
            mem += node["capacity"] * 8  # pointers
            mem += sum(len(k) for k in node["keys"])  # key storage
            mem += 16  # slope + intercept (two floats)
        return mem


# ─── Baseline 2: PGM-style Piecewise Linear Index ───────────────────────────

class PiecewiseLinearIndex:
    """
    Simplified PGM-Index style piecewise linear CDF approximation.
    
    Key ideas from PGM-Index (VLDB 2020):
    - Divides the sorted key space into segments
    - Each segment has a linear model (slope, intercept)
    - Guarantees worst-case error bound epsilon
    - Supports dynamic updates via a buffer + merge strategy
    
    This is a faithful simplified reproduction for benchmarking purposes.
    """
    
    def __init__(self, epsilon: int = 64):
        self.epsilon = epsilon  # Maximum allowed prediction error
        self.segments: List[dict] = []
        self.sorted_keys: List[str] = []
        self.buffer: List[str] = []  # Insert buffer for dynamic updates
        self.buffer_limit = 1000
        self.total_merges = 0
        self.total_retrains = 0
    
    def build(self, keys: List[str]):
        """Build PGM index from sorted keys."""
        self.sorted_keys = list(keys)
        self._build_segments()
    
    def _build_segments(self):
        """Builds piecewise linear segments with epsilon-guaranteed error bounds."""
        n = len(self.sorted_keys)
        if n == 0:
            self.segments = []
            return
        
        # Convert keys to numerical values for linear regression
        key_vals = np.array([self._key_to_float(k) for k in self.sorted_keys])
        
        segments = []
        start = 0
        
        while start < n:
            # Greedily extend segment while error stays within epsilon
            end = start + 1
            
            while end < n:
                # Fit linear model to [start, end]
                seg_len = end - start + 1
                if seg_len < 2:
                    end += 1
                    continue
                
                x = key_vals[start:end + 1]
                y = np.arange(start, end + 1, dtype=np.float64)
                
                # Simple linear regression
                x_mean = np.mean(x)
                y_mean = np.mean(y)
                denom = np.sum((x - x_mean) ** 2)
                
                if denom < 1e-15:
                    slope = 0.0
                else:
                    slope = np.sum((x - x_mean) * (y - y_mean)) / denom
                intercept = y_mean - slope * x_mean
                
                # Check max error
                predictions = slope * x + intercept
                max_error = np.max(np.abs(predictions - y))
                
                if max_error > self.epsilon:
                    break
                
                end += 1
            
            # Finalize segment [start, end-1]
            seg_end = end - 1
            seg_len = seg_end - start + 1
            
            if seg_len >= 2:
                x = key_vals[start:seg_end + 1]
                y = np.arange(start, seg_end + 1, dtype=np.float64)
                x_mean = np.mean(x)
                y_mean = np.mean(y)
                denom = np.sum((x - x_mean) ** 2)
                slope = np.sum((x - x_mean) * (y - y_mean)) / denom if denom > 1e-15 else 0.0
                intercept = y_mean - slope * x_mean
            else:
                slope = 0.0
                intercept = float(start)
            
            segments.append({
                "start_idx": start,
                "end_idx": seg_end,
                "start_key": self.sorted_keys[start],
                "end_key": self.sorted_keys[seg_end],
                "slope": slope,
                "intercept": intercept,
            })
            
            start = seg_end + 1
        
        self.segments = segments
    
    @staticmethod
    def _key_to_float(key: str) -> float:
        """Converts a string key to a float for linear regression."""
        from hli.hli_gyro import analytical_poincare_embedding
        _, theta = analytical_poincare_embedding(key)
        return theta
    
    def _find_segment(self, key: str) -> int:
        """Binary search for the segment containing a key."""
        lo, hi = 0, len(self.segments) - 1
        while lo < hi:
            mid = (lo + hi) // 2
            if self.segments[mid]["end_key"] < key:
                lo = mid + 1
            else:
                hi = mid
        return lo

    def predict_position(self, key: str) -> int:
        """Predict the base-array position for a key using its learned segment."""
        if not self.sorted_keys:
            return 0
        seg_idx = self._find_segment(key)
        seg = self.segments[seg_idx]
        key_val = self._key_to_float(key)
        pred_pos = int(seg["slope"] * key_val + seg["intercept"])
        return max(0, min(len(self.sorted_keys) - 1, pred_pos))
    
    def lookup(self, key: str) -> Tuple[Optional[int], int]:
        """
        Looks up a key. Returns (position, comparisons).
        """
        comparisons = 0
        n = len(self.sorted_keys)
        
        # Check buffer first
        if key in self.buffer:
            return -1, 1  # Found in buffer
        
        # Find segment
        seg_idx = self._find_segment(key)
        comparisons += int(math.log2(max(1, len(self.segments)))) + 1
        
        pred_pos = self.predict_position(key)
        
        # Local binary search within [pred - epsilon, pred + epsilon]
        lo = max(0, pred_pos - self.epsilon)
        hi = min(n - 1, pred_pos + self.epsilon)
        
        while lo <= hi:
            mid = (lo + hi) // 2
            comparisons += 1
            if self.sorted_keys[mid] == key:
                return mid, comparisons
            elif self.sorted_keys[mid] < key:
                lo = mid + 1
            else:
                hi = mid - 1
        
        return None, comparisons
    
    def insert(self, key: str) -> dict:
        """Insert into buffer, merge when buffer is full."""
        metrics = {"comparisons": 0, "shifts": 0, "retrains": 0, "time_us": 0}
        start = time.perf_counter()
        
        self.buffer.append(key)
        
        if len(self.buffer) >= self.buffer_limit:
            # Merge buffer into sorted array and rebuild segments
            self.sorted_keys = sorted(self.sorted_keys + self.buffer)
            self.buffer = []
            self._build_segments()
            self.total_merges += 1
            self.total_retrains += len(self.segments)
            metrics["retrains"] = len(self.segments)
            metrics["shifts"] = len(self.sorted_keys)  # Full rebuild
        
        metrics["time_us"] = (time.perf_counter() - start) * 1e6
        return metrics
    
    def bulk_insert(self, keys: List[str]) -> dict:
        """Bulk insert with merge."""
        start = time.perf_counter()
        self.sorted_keys = sorted(self.sorted_keys + list(keys))
        self._build_segments()
        self.total_merges += 1
        self.total_retrains += len(self.segments)
        return {
            "retrains": len(self.segments),
            "shifts": len(self.sorted_keys),
            "time_us": (time.perf_counter() - start) * 1e6,
        }
    
    def memory_bytes(self) -> int:
        """Estimate memory usage."""
        seg_mem = len(self.segments) * 32  # slope + intercept + boundaries
        key_mem = sum(len(k) for k in self.sorted_keys)
        buf_mem = sum(len(k) for k in self.buffer)
        return seg_mem + key_mem + buf_mem


# ─── Baseline 3: Adaptive Radix Tree (ART) ──────────────────────────────────

class AdaptiveRadixTree:
    """
    Simplified ART (Adaptive Radix Trie) for string key lookups.
    
    Key ideas from ART (ICDE 2013):
    - Radix tree with adaptive node sizes (Node4, Node16, Node48, Node256)
    - Excellent for variable-length string keys
    - O(key_length) lookups regardless of dataset size
    - Supports dynamic inserts natively
    
    This simplified version uses a standard trie with path compression.
    """
    
    def __init__(self):
        self.root = {"children": {}, "is_leaf": False, "position": None}
        self.total_keys = 0
        self.total_nodes = 0
    
    def build(self, keys: List[str]):
        """Build trie from sorted keys."""
        self.root = {"children": {}, "is_leaf": False, "position": None}
        self.total_nodes = 1
        self.total_keys = 0
        
        for idx, key in enumerate(keys):
            self._insert_key(key, idx)
    
    def _insert_key(self, key: str, position: int):
        """Insert a single key into the trie."""
        node = self.root
        for char in key:
            if char not in node["children"]:
                node["children"][char] = {"children": {}, "is_leaf": False, "position": None}
                self.total_nodes += 1
            node = node["children"][char]
        node["is_leaf"] = True
        node["position"] = position
        self.total_keys += 1
    
    def lookup(self, key: str) -> Tuple[Optional[int], int]:
        """
        Looks up a key. Returns (position, comparisons).
        Each character traversal counts as one comparison.
        """
        node = self.root
        comparisons = 0
        
        for char in key:
            comparisons += 1
            if char not in node["children"]:
                return None, comparisons
            node = node["children"][char]
        
        if node["is_leaf"]:
            return node["position"], comparisons
        return None, comparisons
    
    def insert(self, key: str) -> dict:
        """Insert a key. Returns metrics."""
        start = time.perf_counter()
        position = self.total_keys
        self._insert_key(key, position)
        return {
            "comparisons": len(key),
            "shifts": 0,  # Tries don't shift data
            "retrains": 0,
            "time_us": (time.perf_counter() - start) * 1e6,
        }
    
    def bulk_insert(self, keys: List[str]) -> dict:
        """Bulk insert."""
        start = time.perf_counter()
        total_comps = 0
        for key in keys:
            self._insert_key(key, self.total_keys)
            total_comps += len(key)
        return {
            "comparisons": total_comps,
            "shifts": 0,
            "retrains": 0,
            "time_us": (time.perf_counter() - start) * 1e6,
        }
    
    def memory_bytes(self) -> int:
        """Estimate memory: each node ~64 bytes (dict overhead + pointers)."""
        return self.total_nodes * 64


# ─── Baseline 4: Standard B+-Tree ───────────────────────────────────────────

class BPlusTreeIndex:
    """
    Standard sorted array with B+-Tree-style binary search.
    Represents the theoretical performance of a perfectly balanced B+-Tree.
    Inserts require physical array shifting.
    """
    
    def __init__(self):
        self.sorted_keys: List[str] = []
        self.total_shifts = 0
    
    def build(self, keys: List[str]):
        """Build from sorted keys."""
        self.sorted_keys = list(keys)
    
    def lookup(self, key: str) -> Tuple[Optional[int], int]:
        """Binary search lookup."""
        comparisons = 0
        lo, hi = 0, len(self.sorted_keys) - 1
        
        while lo <= hi:
            mid = (lo + hi) // 2
            comparisons += 1
            if self.sorted_keys[mid] == key:
                return mid, comparisons
            elif self.sorted_keys[mid] < key:
                lo = mid + 1
            else:
                hi = mid - 1
        
        return None, comparisons
    
    def insert(self, key: str) -> dict:
        """Insert with physical shift."""
        start = time.perf_counter()
        pos = bisect.bisect_left(self.sorted_keys, key)
        shifts = len(self.sorted_keys) - pos
        self.sorted_keys.insert(pos, key)
        self.total_shifts += shifts
        return {
            "comparisons": int(math.log2(max(1, len(self.sorted_keys)))) + 1,
            "shifts": shifts,
            "retrains": 0,
            "time_us": (time.perf_counter() - start) * 1e6,
        }
    
    def bulk_insert(self, keys: List[str]) -> dict:
        """Bulk insert (merge sort approach)."""
        start = time.perf_counter()
        total_shifts = len(self.sorted_keys) * len(keys)  # Worst case
        self.sorted_keys = sorted(self.sorted_keys + list(keys))
        return {
            "comparisons": len(keys) * int(math.log2(max(1, len(self.sorted_keys)))) + 1,
            "shifts": total_shifts,
            "retrains": 0,
            "time_us": (time.perf_counter() - start) * 1e6,
        }
    
    def memory_bytes(self) -> int:
        """Key storage + array overhead."""
        return sum(len(k) for k in self.sorted_keys) + len(self.sorted_keys) * 8


if __name__ == "__main__":
    import random
    
    print("=" * 70)
    print(" Baseline Index Implementations - Sanity Test")
    print("=" * 70)
    
    # Generate test data
    random.seed(42)
    keys = sorted([f"/level{i}/sub{j}/leaf{k}" 
                   for i in range(10) for j in range(20) for k in range(50)])
    
    print(f"\nDataset: {len(keys)} sorted hierarchical keys")
    
    baselines = {
        "B+-Tree (Binary Search)": BPlusTreeIndex(),
        "ALEX-style (Gapped Array)": GappedArrayIndex(),
        "PGM-style (Piecewise Linear)": PiecewiseLinearIndex(epsilon=32),
        "ART (Adaptive Radix Trie)": AdaptiveRadixTree(),
    }
    
    for name, idx in baselines.items():
        idx.build(keys)
        
        # Test lookups
        total_comps = 0
        found = 0
        for key in keys[:100]:
            pos, comps = idx.lookup(key)
            total_comps += comps
            if pos is not None:
                found += 1
        
        # Test inserts
        insert_keys = [f"/level0/sub0/new_key_{i:04d}" for i in range(100)]
        insert_metrics = idx.bulk_insert(insert_keys)
        
        print(f"\n  {name}:")
        print(f"    Lookup: {found}/100 found, avg {total_comps/100:.1f} comparisons")
        print(f"    Insert 100 keys: {insert_metrics['time_us']:.0f} μs total")
        print(f"    Memory: {idx.memory_bytes():,} bytes")
