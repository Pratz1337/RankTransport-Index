import unittest

from hli.lexicode import compare_lexicode, sparse_lex_codes


class LexicodeTests(unittest.TestCase):
    def test_sparse_lexicode_matches_ascii_path_order(self):
        paths = [
            "/root",
            "/root/a",
            "/root/a/z",
            "/root/aa/z",
            "/root/ab",
            "/root/aaaaaaaa0",
            "/root/aaaaaaaa0/z",
            "/root/aaaaaaaa1",
            "/root/sameprefix000",
            "/root/sameprefix001",
            "/root/z",
        ]
        self.assertEqual(sorted(paths, key=sparse_lex_codes), sorted(paths))

    def test_old_apnpe_counterexamples_are_fixed_by_exact_lexicode(self):
        pairs = [
            ("/root/aa/z", "/root/ab"),
            ("/root/aaaaaaaa0/z", "/root/aaaaaaaa1"),
        ]
        for left, right in pairs:
            self.assertLess(left, right)
            self.assertLess(compare_lexicode(left, right), 0)

    def test_exact_code_distinguishes_long_common_prefixes(self):
        left = "/root/sameprefix000"
        right = "/root/sameprefix001"
        self.assertLess(left, right)
        self.assertLess(compare_lexicode(left, right), 0)


if __name__ == "__main__":
    unittest.main()
