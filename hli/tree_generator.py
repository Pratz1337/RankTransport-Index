import random
import collections

class TreeNode:
    def __init__(self, node_id, path, depth, parent_id=None):
        self.node_id = node_id
        self.path = path
        self.depth = depth
        self.parent_id = parent_id
        self.children_ids = []

    def __repr__(self):
        return f"TreeNode(id={self.node_id}, path={self.path}, depth={self.depth})"

def generate_hierarchical_data(num_nodes=5000, max_depth=5, min_branching=2, max_branching=5):
    """
    Generates a synthetic hierarchical dataset of paths.
    Returns a dictionary of node_id -> TreeNode, and a sorted list of all nodes.
    """
    # Use a set of English-like syllables or words to make paths readable
    words = [
        "data", "sys", "net", "web", "cloud", "core", "node", "base", "app", 
        "user", "auth", "dev", "test", "prod", "api", "query", "index", "cache",
        "store", "log", "task", "job", "worker", "cluster", "graph", "tree",
        "search", "sort", "hash", "crypt", "secure", "media", "image", "video"
    ]
    
    random.seed(42)  # For reproducibility
    
    nodes = {}
    # Create root
    root_node = TreeNode(node_id=0, path="/root", depth=0)
    nodes[0] = root_node
    
    current_node_count = 1
    queue = collections.deque([0])
    
    while queue and current_node_count < num_nodes:
        parent_id = queue.popleft()
        parent_node = nodes[parent_id]
        
        if parent_node.depth >= max_depth:
            continue
            
        # Determine number of children
        num_children = random.randint(min_branching, max_branching)
        # Avoid overshoot
        num_children = min(num_children, num_nodes - current_node_count)
        
        used_words = set()
        for _ in range(num_children):
            # Generate a unique path segment among siblings
            word = random.choice(words)
            while word in used_words:
                word = random.choice(words) + str(random.randint(1, 9))
            used_words.add(word)
            
            child_path = f"{parent_node.path}/{word}"
            child_id = current_node_count
            child_node = TreeNode(node_id=child_id, path=child_path, depth=parent_node.depth + 1, parent_id=parent_id)
            
            nodes[child_id] = child_node
            parent_node.children_ids.append(child_id)
            
            queue.append(child_id)
            current_node_count += 1
            
            if current_node_count >= num_nodes:
                break
                
    # Sort nodes lexicographically by their path
    # In database indexing, keys are sorted alphabetically
    sorted_nodes = sorted(nodes.values(), key=lambda node: node.path)
    
    # Assign physical array offsets and ground-truth normalized CDF values
    for idx, node in enumerate(sorted_nodes):
        node.physical_index = idx
        node.cdf = idx / (len(sorted_nodes) - 1)
        
    return nodes, sorted_nodes

if __name__ == "__main__":
    nodes, sorted_nodes = generate_hierarchical_data(100, 3, 2, 4)
    print(f"Generated {len(nodes)} nodes.")
    print("First 10 sorted paths:")
    for n in sorted_nodes[:10]:
        print(f"  Index {n.physical_index} (CDF {n.cdf:.3f}): {n.path}")
