import numpy as np
import torch

def exact_lookup_hli(model, query_node, sorted_nodes, input_features, error_bound):
    """
    Performs an exact key lookup using the Hyperbolic Learned Index (HLI).
    1. Uses the trained HLI neural network to predict the position.
    2. Performs a localized binary search within the mathematically verified error bound.
    
    Returns:
      found_node: The retrieved TreeNode, or None if not found.
      comparisons: The number of key comparisons (array accesses) performed.
    """
    N = len(sorted_nodes)
    
    # 1. Run inference on the model
    with torch.no_grad():
        pred_cdf = model(input_features.unsqueeze(0)).item()
        
    # Scale normalized prediction to array index
    pred_idx = int(round(pred_cdf * (N - 1)))
    pred_idx = max(0, min(N - 1, pred_idx))
    
    # 2. Localized binary search within the error bound [pred_idx - error_bound, pred_idx + error_bound]
    low = max(0, pred_idx - error_bound)
    high = min(N - 1, pred_idx + error_bound)
    
    comparisons = 0
    found_node = None
    
    target_path = query_node.path
    
    while low <= high:
        mid = (low + high) // 2
        mid_node = sorted_nodes[mid]
        comparisons += 1
        
        if mid_node.path == target_path:
            found_node = mid_node
            break
        elif mid_node.path < target_path:
            low = mid + 1
        else:
            high = mid - 1
            
    return found_node, comparisons, pred_idx

def exact_lookup_baseline(model, query_node, sorted_nodes, input_feature, error_bound):
    """
    Performs an exact key lookup using the Euclidean Learned Index baseline.
    """
    N = len(sorted_nodes)
    
    with torch.no_grad():
        pred_cdf = model(input_feature.unsqueeze(0)).item()
        
    pred_idx = int(round(pred_cdf * (N - 1)))
    pred_idx = max(0, min(N - 1, pred_idx))
    
    low = max(0, pred_idx - error_bound)
    high = min(N - 1, pred_idx + error_bound)
    
    comparisons = 0
    found_node = None
    
    target_path = query_node.path
    
    while low <= high:
        mid = (low + high) // 2
        mid_node = sorted_nodes[mid]
        comparisons += 1
        
        if mid_node.path == target_path:
            found_node = mid_node
            break
        elif mid_node.path < target_path:
            low = mid + 1
        else:
            high = mid - 1
            
    return found_node, comparisons, pred_idx

def binary_search_standard(query_node, sorted_nodes):
    """
    Standard full binary search baseline (represents a perfectly balanced B-Tree).
    """
    N = len(sorted_nodes)
    low = 0
    high = N - 1
    
    comparisons = 0
    found_node = None
    target_path = query_node.path
    
    while low <= high:
        mid = (low + high) // 2
        mid_node = sorted_nodes[mid]
        comparisons += 1
        
        if mid_node.path == target_path:
            found_node = mid_node
            break
        elif mid_node.path < target_path:
            low = mid + 1
        else:
            high = mid - 1
            
    return found_node, comparisons

def subtree_range_query(start_node, sorted_nodes, exact_lookup_fn, model, input_features, error_bound):
    """
    Executes a subtree range query (finds all descendants of start_node).
    Since keys are sorted lexicographically, descendants are contiguous in the sorted array.
    We find the start index via HLI, and then perform a local prefix binary search to find the end boundary.
    
    Returns:
      results: List of TreeNodes in the subtree
      total_comparisons: Sum of comparisons to find boundaries.
    """
    N = len(sorted_nodes)
    
    # 1. Find start node index using HLI exact lookup
    retrieved_node, start_comparisons, _ = exact_lookup_fn(model, start_node, sorted_nodes, input_features, error_bound)
    if retrieved_node is None:
        return [], start_comparisons
        
    start_idx = retrieved_node.physical_index
    prefix = start_node.path + "/"
    
    # 2. Binary search for the upper bound (end of the prefix)
    # The search range is [start_idx + 1, N - 1]
    low = start_idx
    high = N - 1
    end_idx = start_idx
    
    prefix_comparisons = 0
    
    while low <= high:
        mid = (low + high) // 2
        mid_node = sorted_nodes[mid]
        prefix_comparisons += 1
        
        # Check if mid_node belongs to the subtree (starts with prefix or is the start_node itself)
        if mid_node.path == start_node.path or mid_node.path.startswith(prefix):
            end_idx = mid
            low = mid + 1  # Try to find a larger index
        else:
            high = mid - 1
            
    results = sorted_nodes[start_idx : end_idx + 1]
    total_comparisons = start_comparisons + prefix_comparisons
    
    return results, total_comparisons
