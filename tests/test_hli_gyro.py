import unittest
import math

try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

if HAS_TORCH:
    from hli.hli_gyro import (
        analytical_poincare_embedding,
        log_poincare_map,
        mobius_add,
        mobius_linear,
        mobius_activation,
        GyroMLP
    )

@unittest.skipUnless(HAS_TORCH, "PyTorch is required for Gyro tests")
class GyroTests(unittest.TestCase):
    def test_analytical_embedding_is_bounded_and_deterministic(self):
        # A list of representative paths in sorted lexicographical order.
        paths = [
            "/root",
            "/root/a",
            "/root/a/b",
            "/root/a/b/c",
            "/root/a/c",
            "/root/b",
            "/root/b/a",
            "/root/c",
            "/root/sys",
            "/root/sys/app",
            "/root/sys/app/auth",
            "/root/sys/app/db",
            "/root/sys/kernel",
            "/root/usr",
            "/root/usr/bin",
            "/root/usr/local",
            "/root/usr/local/bin"
        ]
        
        embeddings = [analytical_poincare_embedding(p, alpha=0.5) for p in paths]
        self.assertEqual(embeddings, [analytical_poincare_embedding(p, alpha=0.5) for p in paths])
        for r, theta in embeddings:
            self.assertGreaterEqual(r, 0.0)
            self.assertLess(r, 1.0)
            self.assertGreaterEqual(theta, 0.0)
            self.assertLess(theta, 2.0 * math.pi)

    def test_log_map_properties(self):
        # Verify that radius r is mapped correctly
        r = 0.5
        theta = math.pi / 4.0
        x, y = log_poincare_map(r, theta)
        
        d_H = 2.0 * math.atanh(r)
        self.assertAlmostEqual(math.sqrt(x**2 + y**2), d_H)
        self.assertAlmostEqual(math.atan2(y, x), theta)

    def test_mobius_add_identity(self):
        u = torch.tensor([0.1, -0.2])
        zero = torch.zeros(2)
        
        # u + 0 = u
        res1 = mobius_add(u, zero)
        torch.testing.assert_close(res1, u)
        
        # 0 + u = u
        res2 = mobius_add(zero, u)
        torch.testing.assert_close(res2, u)

    def test_gyro_mlp_runs(self):
        model = GyroMLP(hidden_dim=16, layers=2)
        # 5 paths mapped to Poincare coordinates
        coords = []
        for path in ["/a", "/a/b", "/a/b/c", "/b", "/c"]:
            r, theta = analytical_poincare_embedding(path)
            # convert polar to cartesian Poincare coordinates
            coords.append([r * math.cos(theta), r * math.sin(theta)])
            
        x_tensor = torch.tensor(coords, dtype=torch.float32)
        outputs = model(x_tensor)
        
        self.assertEqual(outputs.shape, (5, 1))
        # Sigmoid bounded outputs in [0, 1]
        self.assertTrue(torch.all(outputs >= 0.0))
        self.assertTrue(torch.all(outputs <= 1.0))

if __name__ == "__main__":
    unittest.main()
