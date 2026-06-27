import numpy as np

def mobius_transform(z, z0):
    """
    Computes the Möbius transformation (conformal automorphism) of the Poincaré disk:
    f(z) = (z - z0) / (1 - conj(z0) * z)
    This transformation analytically stretches the space near z0 and compresses it on the opposite side.
    """
    z = np.asarray(z, dtype=complex)
    z0 = np.asarray(z0, dtype=complex)
    
    # Avoid division by zero
    denom = 1.0 - np.conj(z0) * z
    # Clip denominator to avoid extreme values close to boundary
    denom = np.where(np.abs(denom) < 1e-9, 1e-9, denom)
    
    return (z - z0) / denom

def inverse_mobius_transform(w, z0):
    """
    Computes the inverse Möbius transformation:
    f^(-1)(w) = (w + z0) / (1 + conj(z0) * w)
    """
    w = np.asarray(w, dtype=complex)
    z0 = np.asarray(z0, dtype=complex)
    
    denom = 1.0 + np.conj(z0) * w
    denom = np.where(np.abs(denom) < 1e-9, 1e-9, denom)
    
    return (w + z0) / denom

def compute_insertion_warp(insertion_angle, num_inserted, total_nodes, alpha=0.3):
    """
    Calculates the conformal warp center z0 based on the insertion location and volume.
    The magnitude of z0 (|z0|) determines the degree of space dilation.
    More insertions = larger |z0| = greater space expansion.
    """
    # The scale of the warp center corresponds to the insertion ratio
    ratio = num_inserted / (total_nodes + num_inserted)
    
    # We restrict the warp center magnitude to [0, 0.9] to prevent leaf crowding errors
    magnitude = min(0.9, ratio * alpha)
    
    # The warp center z0 is placed at the insertion angle, pointing inwards
    z0 = magnitude * np.exp(1j * insertion_angle)
    return z0

def apply_conformal_warp(nodes, sorted_nodes, insertion_angle, num_inserted, alpha=0.3):
    """
    Applies the Möbius conformal transformation to all nodes in the database.
    This warps their hyperbolic coordinates to accommodate the inserted nodes conformally.
    """
    N = len(sorted_nodes)
    z0 = compute_insertion_warp(insertion_angle, num_inserted, N, alpha)
    
    # Convert Cartesian coordinates to complex representation
    coords = []
    for node in sorted_nodes:
        coords.append(node.x + 1j * node.y)
    coords = np.array(coords, dtype=complex)
    
    # Apply Möbius transform to warp coordinates
    # We apply f(z) = (z - z0)/(1 - conj(z0)*z)
    warped_coords = mobius_transform(coords, z0)
    
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
        
    return z0

if __name__ == "__main__":
    # Test Möbius transform
    z = 0.5 + 0.0j
    z0 = 0.2 + 0.1j
    w = mobius_transform(z, z0)
    z_back = inverse_mobius_transform(w, z0)
    print(f"Original: {z}")
    print(f"Warped: {w}")
    print(f"Inverted back: {z_back} (Correct: {np.allclose(z, z_back)})")
