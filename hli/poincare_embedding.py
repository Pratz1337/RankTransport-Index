import numpy as np
import collections

def embed_tree_poincare(nodes, root_id=0, alpha=0.6, spacing=0.85):
    """
    Deterministically embeds a hierarchical tree into the 2D Poincaré disk.
    
    Parameters:
      nodes: dict of node_id -> TreeNode
      root_id: ID of the root node
      alpha: parameter controlling radial scaling (r = 1 - e^(-alpha * depth))
      spacing: angular spacing factor in (0, 1) to prevent sibling overlap
      
    Returns:
      embeddings: dict of node_id -> np.array([x, y]) representing 2D coordinates
      angles: dict of node_id -> float (angle in radians)
      wedges: dict of node_id -> float (angular wedge width in radians)
    """
    embeddings = {}
    angles = {}
    wedges = {}
    
    # Root placement at the origin
    embeddings[root_id] = np.array([0.0, 0.0])
    angles[root_id] = 0.0
    wedges[root_id] = 2.0 * np.pi
    
    # BFS to embed level by level
    queue = collections.deque([root_id])
    
    while queue:
        parent_id = queue.popleft()
        parent_node = nodes[parent_id]
        children_ids = parent_node.children_ids
        
        if not children_ids:
            continue
            
        k = len(children_ids)
        parent_angle = angles[parent_id]
        parent_wedge = wedges[parent_id]
        
        # Determine the radius for this depth layer
        depth = parent_node.depth + 1
        r = 1.0 - np.exp(-alpha * depth)
        
        if parent_id == root_id:
            # For the root, distribute children evenly around the circle
            for i, child_id in enumerate(children_ids):
                theta = (2.0 * np.pi * i) / k
                wedge_w = (2.0 * np.pi) / k
                
                x = r * np.cos(theta)
                y = r * np.sin(theta)
                
                embeddings[child_id] = np.array([x, y])
                angles[child_id] = theta
                wedges[child_id] = wedge_w
                queue.append(child_id)
        else:
            # For non-root nodes, partition the parent's angular wedge
            # We shrink the wedge slightly by the spacing factor to create visual/geometric separation
            total_allocated_wedge = spacing * parent_wedge
            child_wedge = total_allocated_wedge / k
            
            # Start angle of the children wedge block
            start_angle = parent_angle - (total_allocated_wedge / 2.0)
            
            for i, child_id in enumerate(children_ids):
                # Place the child in the middle of its allotted sub-wedge
                theta = start_angle + child_wedge * (i + 0.5)
                
                # Normalize theta to [-pi, pi]
                theta = (theta + np.pi) % (2.0 * np.pi) - np.pi
                
                x = r * np.cos(theta)
                y = r * np.sin(theta)
                
                embeddings[child_id] = np.array([x, y])
                angles[child_id] = theta
                wedges[child_id] = child_wedge
                queue.append(child_id)
                
    # Update nodes with their coordinates and geometric features
    for node_id, node in nodes.items():
        coord = embeddings[node_id]
        node.x = float(coord[0])
        node.y = float(coord[1])
        node.radius = float(np.linalg.norm(coord))
        node.theta = float(angles[node_id])
        node.wedge = float(wedges[node_id])
        # Compute hyperbolic distance to the origin d_H(z, 0) = 2 * artanh(||z||)
        # Use clip to avoid log(0) or division by zero at boundary
        norm_z = min(node.radius, 0.9999)
        node.hyperbolic_dist = float(2.0 * np.arctanh(norm_z))
        
    return embeddings

if __name__ == "__main__":
    from tree_generator import generate_hierarchical_data
    nodes, sorted_nodes = generate_hierarchical_data(10, 3, 2, 3)
    embeds = embed_tree_poincare(nodes, alpha=0.5, spacing=0.85)
    print("Embedding coordinates:")
    for n in sorted_nodes:
        print(f"  {n.path:20} -> X: {n.x:.3f}, Y: {n.y:.3f}, Radius: {n.radius:.3f}, Hyperbolic Dist: {n.hyperbolic_dist:.3f}")
