import math
import numpy as np
import torch
import torch.nn as nn

from hli.lexicode import lexicode_unit_float

def component_to_float(s: str, max_chars: int = 8) -> float:
    """Legacy bounded component feature; not an exact order certificate."""
    val = 0.0
    base = 256.0
    for i in range(min(len(s), max_chars)):
        val += ord(s[i]) / (base ** (i + 1))
    return val

def analytical_poincare_embedding(path_str: str, alpha: float = 0.5, B: float = 300.0) -> tuple[float, float]:
    """Embed a hierarchical path as bounded Poincare polar coordinates.

    The angular coordinate is a compact model feature derived from a bounded
    sparse-radix prefix. Exact ordering is provided by hli.lexicode, not by this
    finite-precision angle.
    """
    parts = [p for p in path_str.split('/') if p]
    depth = len(parts)
    if depth == 0:
        return 0.0, 0.0

    r = 1.0 - math.exp(-alpha * depth)
    theta = 0.1 + 5.9 * lexicode_unit_float(path_str)
    return r, theta

def _legacy_analytical_poincare_embedding(path_str: str, alpha: float = 0.5, B: float = 300.0) -> tuple[float, float]:
    """
    Deterministically embeds a hierarchical path string into 2D Poincaré disk coordinates (r, theta)
    in O(L) time without traversing any physical tree.
    """
    parts = [p for p in path_str.split('/') if p]
    depth = len(parts)
    if depth == 0:
        return 0.0, 0.0

    r = 1.0 - math.exp(-alpha * depth)

    # Compute a prefix-nested radix angle
    theta_radix = 0.0
    for i, part in enumerate(parts):
        v = component_to_float(part)
        # Shift to ensure parent (prefix) is strictly smaller than descendants
        theta_radix += (v + 0.1) * (B ** (-i - 1))

    # Scale theta to fit within [0.1, 6.0] to avoid periodic wrap-around
    # Maximum possible theta_radix is \sum_{i=1}^\infty 1.1 * B**(-i) = 1.1 / (B - 1)
    max_possible = 1.1 / (B - 1.0)
    theta = 0.1 + (5.9 / max_possible) * theta_radix

    return r, theta

def log_poincare_map(r: float, theta: float) -> tuple[float, float]:
    """
    Maps 2D Poincaré disk coordinates (r, theta) to tangent space coordinates at the origin (x, y)
    to prevent numerical underflow / precision collapse.
    """
    r_clamped = min(r, 0.999999)
    d_H = 2.0 * math.atanh(r_clamped)
    x = d_H * math.cos(theta)
    y = d_H * math.sin(theta)
    return x, y

def mobius_add(u: torch.Tensor, v: torch.Tensor, c: float = 1.0) -> torch.Tensor:
    """
    Möbius addition in gyrovector space.
    """
    u2 = torch.sum(u * u, dim=-1, keepdim=True)
    v2 = torch.sum(v * v, dim=-1, keepdim=True)
    uv = torch.sum(u * v, dim=-1, keepdim=True)
    num = (1.0 + 2.0 * c * uv + c * v2) * u + (1.0 - c * u2) * v
    denom = 1.0 + 2.0 * c * uv + (c ** 2) * u2 * v2
    return num / torch.clamp(denom, min=1e-15)

def mobius_linear(x: torch.Tensor, weight: torch.Tensor, bias: torch.Tensor, c: float = 1.0) -> torch.Tensor:
    """
    Möbius linear layer: y = (W \otimes_c x) \oplus_c b.
    """
    wx = torch.matmul(x, weight.t())
    wx_norm = torch.norm(wx, dim=-1, keepdim=True)
    x_norm = torch.norm(x, dim=-1, keepdim=True)
    
    x_norm_clamped = torch.clamp(x_norm, min=1e-15)
    wx_norm_clamped = torch.clamp(wx_norm, min=1e-15)
    
    theta = torch.arctanh(torch.clamp(math.sqrt(c) * x_norm, max=0.9999))
    scale = torch.tanh(wx_norm / x_norm_clamped * theta)
    res = (1.0 / math.sqrt(c)) * scale * (wx / wx_norm_clamped)
    
    # Handle the origin case
    res = torch.where(x_norm == 0.0, torch.zeros_like(wx), res)
    
    bias_projected = torch.clamp(bias, -0.9999 / math.sqrt(c), 0.9999 / math.sqrt(c))
    return mobius_add(res, bias_projected, c)

def mobius_activation(x: torch.Tensor, act_fn=torch.relu, c: float = 1.0) -> torch.Tensor:
    """
    Möbius activation function: exp_0(act_fn(log_0(x))).
    """
    x_norm = torch.norm(x, dim=-1, keepdim=True)
    x_norm_clamped = torch.clamp(x_norm, min=1e-15)
    
    # Log map to tangent space
    v = (2.0 / math.sqrt(c)) * torch.arctanh(torch.clamp(math.sqrt(c) * x_norm, max=0.9999)) * (x / x_norm_clamped)
    v = torch.where(x_norm == 0.0, torch.zeros_like(v), v)
    
    v_act = act_fn(v)
    v_act_norm = torch.norm(v_act, dim=-1, keepdim=True)
    v_act_norm_clamped = torch.clamp(v_act_norm, min=1e-15)
    
    # Exp map back to Poincare disk
    res = (1.0 / math.sqrt(c)) * torch.tanh(0.5 * math.sqrt(c) * v_act_norm) * (v_act / v_act_norm_clamped)
    return torch.where(v_act_norm == 0.0, torch.zeros_like(res), res)

class GyroLinear(nn.Module):
    def __init__(self, in_features: int, out_features: int, c: float = 1.0):
        super().__init__()
        self.in_features = in_features
        self.out_features = out_features
        self.c = c
        self.weight = nn.Parameter(torch.Tensor(out_features, in_features))
        self.bias = nn.Parameter(torch.Tensor(out_features))
        self.reset_parameters()

    def reset_parameters(self):
        nn.init.xavier_uniform_(self.weight)
        nn.init.uniform_(self.bias, -0.05, 0.05)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return mobius_linear(x, self.weight, self.bias, self.c)

class GyroMLP(nn.Module):
    """
    Legacy hyperbolic feature regressor used in HRT-LI experiments.
    Takes 2D Poincaré disk coordinates (x, y) and performs gyrovector operations
    before mapping to a predicted CDF.
    """
    def __init__(self, hidden_dim: int = 64, layers: int = 2, c: float = 1.0):
        super().__init__()
        self.c = c
        self.first_layer = GyroLinear(2, hidden_dim, c)
        self.hidden_layers = nn.ModuleList([
            GyroLinear(hidden_dim, hidden_dim, c) for _ in range(layers - 1)
        ])
        # Output layer maps from Poincare disk to Euclidean space using the Log map
        self.out_layer = nn.Linear(hidden_dim, 1)
        self.sigmoid = nn.Sigmoid()

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # Input x: shape (..., 2) inside Poincare disk
        h = self.first_layer(x)
        h = mobius_activation(h, torch.relu, self.c)
        for layer in self.hidden_layers:
            h = layer(h)
            h = mobius_activation(h, torch.relu, self.c)
            
        # Log map to tangent space at the origin
        h_norm = torch.norm(h, dim=-1, keepdim=True)
        h_norm_clamped = torch.clamp(h_norm, min=1e-15)
        v = (2.0 / math.sqrt(self.c)) * torch.arctanh(torch.clamp(math.sqrt(self.c) * h_norm, max=0.9999)) * (h / h_norm_clamped)
        v = torch.where(h_norm == 0.0, torch.zeros_like(v), v)
        
        # Euclidean output projection
        out = self.out_layer(v)
        return self.sigmoid(out)
