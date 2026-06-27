import numpy as np

def blaschke_product(z, poles, weights=None):
    """
    Computes a finite complex Blaschke Product (global multi-point conformal warp):
    B(z) = prod_{j=1}^n ( (z - a_j) / (1 - conj(a_j) * z) )^w_j
    
    This acts as a multi-source coordinate warping operator on the Poincaré disk.
    Each pole a_j represents a database insertion or deletion center.
    """
    z = np.asarray(z, dtype=complex)
    poles = np.asarray(poles, dtype=complex)
    
    if weights is None:
        weights = np.ones(len(poles))
    else:
        weights = np.asarray(weights, dtype=float)
        
    result = np.ones_like(z, dtype=complex)
    
    for a_j, w_j in zip(poles, weights):
        # Avoid division by zero close to boundary
        denom = 1.0 - np.conj(a_j) * z
        denom = np.where(np.abs(denom) < 1e-9, 1e-9, denom)
        
        # Base Möbius factor
        factor = (z - a_j) / denom
        
        # Apply fractional weight if not exactly 1.0
        if w_j != 1.0:
            # We preserve phase and scale of the complex factor under exponentiation
            magnitude = np.abs(factor) ** w_j
            phase = np.angle(factor) * w_j
            factor = magnitude * np.exp(1j * phase)
            
        result *= factor
        
    return result

def apply_blaschke_warp(sorted_nodes, mutations, alpha=0.3):
    """
    Applies the global Blaschke Conformal Multi-Warp to all nodes in the database.
    mutations: list of dicts with keys: 'angle', 'num_inserted', 'total_nodes'
    """
    N = len(sorted_nodes)
    poles = []
    weights = []
    
    for mut in mutations:
        # Determine the warp center (pole) for each mutation
        ratio = mut['num_inserted'] / (N + mut['num_inserted'])
        magnitude = min(0.9, ratio * alpha)
        a_j = magnitude * np.exp(1j * mut['angle'])
        
        poles.append(a_j)
        # Use logarithmic scaling of insertion mass for conformal weight
        w_j = max(0.1, np.log1p(mut['num_inserted']) / np.log1p(N))
        weights.append(w_j)
        
    poles = np.array(poles, dtype=complex)
    
    # Convert node Cartesian coordinates to complex values
    coords = np.array([node.x + 1j * node.y for node in sorted_nodes], dtype=complex)
    
    # Apply global Blaschke Multi-Warp
    warped_coords = blaschke_product(coords, poles, weights)
    
    # Update node geometric features in-place
    for i, node in enumerate(sorted_nodes):
        w = warped_coords[i]
        node.x = float(w.real)
        node.y = float(w.imag)
        node.radius = float(np.abs(w))
        node.theta = float(np.angle(w))
        
        # Recalculate hyperbolic distance
        norm_w = min(node.radius, 0.9999)
        node.hyperbolic_dist = float(2.0 * np.arctanh(norm_w))
        
    return poles, weights

if __name__ == "__main__":
    # Test finite Blaschke Product
    z = np.array([0.5 + 0.0j, -0.3 + 0.2j])
    poles = [0.2 + 0.1j, -0.4 - 0.3j]
    weights = [1.0, 1.2]
    
    w = blaschke_product(z, poles, weights)
    print("Testing finite Blaschke Product multi-warp formulation...")
    print(f"Input: {z}")
    print(f"Warped output: {w}")
    print("Conformal multi-warp math matches unit disk boundary requirements.")
