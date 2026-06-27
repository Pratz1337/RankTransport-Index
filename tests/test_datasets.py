import os
import json
import tempfile
import unittest

from hli.datasets import (
    dataset_stats,
    generate_package_manager_paths,
    generate_url_paths,
    load_dns_paths_from_urls,
    load_filesystem_paths,
    load_json_document_paths,
    load_url_paths,
)


class DatasetExpansionTests(unittest.TestCase):
    def test_url_paths_are_hierarchical_and_unique(self):
        paths = generate_url_paths(250, seed=7)
        self.assertEqual(len(paths), 250)
        self.assertEqual(len(paths), len(set(paths)))
        self.assertTrue(all(path.startswith("/url/") for path in paths))
        self.assertGreaterEqual(dataset_stats(paths)["max_depth"], 5)

    def test_package_manager_paths_cover_expected_prefix(self):
        paths = generate_package_manager_paths(300, seed=9)
        self.assertEqual(len(paths), 300)
        self.assertTrue(any(path.startswith("/pkg/npm/") for path in paths))
        self.assertTrue(any(path.startswith("/pkg/pypi/") for path in paths))
        self.assertTrue(any(path.startswith("/pkg/maven/") for path in paths))
        self.assertTrue(any(path.startswith("/pkg/crates/") for path in paths))

    def test_filesystem_loader_reads_local_tree(self):
        paths = load_filesystem_paths(os.getcwd(), max_paths=25)
        self.assertGreater(len(paths), 0)
        self.assertLessEqual(len(paths), 25)
        self.assertTrue(all(path.startswith("/fs/") for path in paths))

    def test_real_url_dns_and_json_loaders_parse_local_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "refs.md"), "w", encoding="utf-8") as handle:
                handle.write("See https://arxiv.org/abs/2407.11556 and https://www.vldb.org/pvldb/vol16/p1992-li.pdf")
            with open(os.path.join(tmp, "sample.json"), "w", encoding="utf-8") as handle:
                json.dump({"users": [{"profile": {"id": 1}}]}, handle)

            urls = load_url_paths(tmp, max_paths=10)
            dns = load_dns_paths_from_urls(tmp, max_paths=10)
            json_paths = load_json_document_paths(tmp, max_paths=10)

        self.assertTrue(any(path.startswith("/url/org/arxiv/abs/") for path in urls))
        self.assertTrue(any(path.startswith("/dns/org/arxiv") for path in dns))
        self.assertIn("/json/sample/users/[]/profile/id", json_paths)


if __name__ == "__main__":
    unittest.main()
