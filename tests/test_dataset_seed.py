"""The synthetic-tree seed must control every random choice."""
import random
import unittest

from hli.datasets import generate_scalable_tree


class SyntheticTreeSeedTests(unittest.TestCase):
    def setUp(self):
        self.global_state = random.getstate()

    def tearDown(self):
        random.setstate(self.global_state)

    def test_same_seed_ignores_global_rng_state(self):
        random.seed(1)
        first = generate_scalable_tree(2200, max_depth=8, seed=42)
        random.seed(2)
        second = generate_scalable_tree(2200, max_depth=8, seed=42)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 2200)
        self.assertEqual(first, sorted(set(first)))

    def test_generation_does_not_consume_global_rng(self):
        generate_scalable_tree(2200, max_depth=8, seed=42)
        self.assertEqual(random.getstate(), self.global_state)

    def test_different_seeds_change_tree(self):
        first = generate_scalable_tree(2200, max_depth=8, seed=42)
        second = generate_scalable_tree(2200, max_depth=8, seed=43)
        self.assertNotEqual(first, second)


if __name__ == "__main__":
    unittest.main()
