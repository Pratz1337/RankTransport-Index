"""Check exported file identities and recorded experiment provenance; no reruns."""
from pathlib import Path
import hashlib
import json


ROOT = Path(__file__).resolve().parents[1]


def verify(relative, expected):
    path = ROOT / relative
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f"SHA-256 mismatch: {relative}")


provenance = json.loads((ROOT / "docs/BUILD_PROVENANCE.json").read_text())
for row in provenance["rows"]:
    verify(row["record"], row["record_sha256"])

for relative in ("hrtli_cpp/packed_rank_transport.hpp",
                 "hrtli_cpp/prefix_radix_delta.hpp",
                 "scripts/prepare_commoncrawl_200m.py",
                 "results_q1/reviewer_response_20260906/query_descriptive.json"):
    path = ROOT / relative
    if not path.is_file():
        raise SystemExit(f"Missing required release file: {relative}")

print(f"PASS: {len(provenance['rows'])} experiment-record digests and required release files")
print("Digest validation does not rerun experiments or validate their conclusions.")
