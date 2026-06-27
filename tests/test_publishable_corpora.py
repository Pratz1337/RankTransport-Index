import unittest

from download_publishable_corpora import (
    _collect_json_paths,
    _normalise_dns_rule,
    _normalise_purl,
    _normalise_url,
)


class PublishableCorpusTests(unittest.TestCase):
    def test_url_normalisation_reverses_host_and_keeps_path(self):
        key = _normalise_url("https://www.example.org/a/b?z=1&a=2")
        self.assertEqual(key, "/url/https/org/example/www/a/b/query/a/z")

    def test_dns_rule_normalisation_handles_wildcards_and_exceptions(self):
        self.assertEqual(_normalise_dns_rule("*.ck"), "/dns/ck")
        self.assertEqual(_normalise_dns_rule("!www.ck"), "/dns/ck/www")
        self.assertIsNone(_normalise_dns_rule("// comment"))

    def test_purl_normalisation_preserves_package_hierarchy(self):
        key = _normalise_purl("pkg:npm/%40angular/animation@12.3.1?repository_url=x#src/index.js")
        self.assertEqual(
            key,
            "/pkg/npm/@angular/animation/version/12.3.1/qualifier/repository_url/subpath/src/index.js",
        )

    def test_json_path_collection_uses_stable_array_placeholder(self):
        out: list[str] = []
        _collect_json_paths({"users": [{"profile": {"id": 1}}]}, "/json/sample", out)
        self.assertIn("/json/sample/users/[0000]/profile/id", out)


if __name__ == "__main__":
    unittest.main()
