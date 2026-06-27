import torch
import torch.nn as nn
import numpy as np

def string_to_float(s, max_chars=12):
    """
    Converts a path string like '/root/sys/node' into a high-precision numerical 1D float
    representing its lexicographical value. This is standard in learned indexing (e.g., RMI).
    """
    val = 0.0
    base = 256.0
    s_clean = s.lstrip('/')
    for i in range(min(len(s_clean), max_chars)):
        val += ord(s_clean[i]) / (base ** (i + 1))
    return val

class EuclideanMLP(nn.Module):
    """
    Baseline 1D Learned Index model.
    Takes the 1D string-to-float representation of a path and predicts its sorted CDF.
    """
    def __init__(self, hidden_dim=64, layers=2):
        super().__init__()
        net = []
        net.append(nn.Linear(1, hidden_dim))
        net.append(nn.ReLU())
        for _ in range(layers - 1):
            net.append(nn.Linear(hidden_dim, hidden_dim))
            net.append(nn.ReLU())
        net.append(nn.Linear(hidden_dim, 1))
        # Keep predictions bounded to [0, 1] using a Sigmoid
        net.append(nn.Sigmoid())
        self.network = nn.Sequential(*net)
        
    def forward(self, x):
        return self.network(x)

class HyperbolicMLP(nn.Module):
    """
    Proposed Hyperbolic Learned Index (HLI) model.
    Takes 2D Poincaré coordinates (x, y) along with hyperbolic inductive biases:
    - Polar coordinates (r, theta)
    - Hyperbolic distance to the origin (d_H)
    This feature expansion gives the neural net direct geometric awareness of the hierarchy.
    """
    def __init__(self, hidden_dim=64, layers=2):
        super().__init__()
        # Inputs: x, y, r, theta, hyperbolic_dist (5 dimensions)
        net = []
        net.append(nn.Linear(5, hidden_dim))
        net.append(nn.ReLU())
        for _ in range(layers - 1):
            net.append(nn.Linear(hidden_dim, hidden_dim))
            net.append(nn.ReLU())
        net.append(nn.Linear(hidden_dim, 1))
        net.append(nn.Sigmoid())
        self.network = nn.Sequential(*net)
        
    def forward(self, x):
        return self.network(x)

def prepare_datasets(sorted_nodes):
    """
    Prepares PyTorch tensors for both Euclidean and Hyperbolic models.
    """
    N = len(sorted_nodes)
    
    # 1. Euclidean inputs (1D string-to-float)
    x_euclid = np.array([string_to_float(node.path) for node in sorted_nodes], dtype=np.float32)
    # Normalize inputs to [0, 1] to assist neural network convergence
    x_euclid_min, x_euclid_max = x_euclid.min(), x_euclid.max()
    if x_euclid_max > x_euclid_min:
        x_euclid = (x_euclid - x_euclid_min) / (x_euclid_max - x_euclid_min)
    else:
        x_euclid = np.zeros_like(x_euclid)
    x_euclid = torch.tensor(x_euclid, dtype=torch.float32).unsqueeze(1)
    
    # 2. Hyperbolic inputs (5D)
    x_hyper = []
    for node in sorted_nodes:
        x_hyper.append([
            node.x,
            node.y,
            node.radius,
            node.theta,
            node.hyperbolic_dist
        ])
    x_hyper = torch.tensor(x_hyper, dtype=torch.float32)
    
    # 3. Targets (CDF in [0, 1])
    y = torch.tensor([node.cdf for node in sorted_nodes], dtype=torch.float32).unsqueeze(1)
    
    return x_euclid, x_hyper, y

def train_model(model, inputs, targets, epochs=300, lr=0.005, verbose=True):
    """
    Trains a neural network model using MSE loss.
    """
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)
    # Use Huber Loss for robustness to outliers near boundaries
    criterion = nn.HuberLoss(delta=0.01)
    
    model.train()
    for epoch in range(epochs):
        optimizer.zero_grad()
        outputs = model(inputs)
        loss = criterion(outputs, targets)
        loss.backward()
        optimizer.step()
        
        if verbose and (epoch + 1) % 50 == 0:
            print(f"  Epoch {epoch+1}/{epochs} | Loss: {loss.item():.6f}")
            
    model.eval()
    return model
