"""Verify the retained natural-key preparation and freeze dependency identities."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("corpus", type=Path)
parser.add_argument("records", type=Path)
args = parser.parse_args()
metadata = json.loads((args.corpus / "provenance.json").read_text())
assert metadata["sample_keys"] == 10000000
assert metadata["base_keys"] == 8000000 and metadata["arrival_keys"] == 2000000
assert metadata["natural_keys_not_generated"] and metadata["full_parent_order_and_hashes_verified"]
assert metadata["parent_selected_keys"] == 200000000 and metadata["sample_stride"] == 20
profile = {}
for name, expected in metadata["files"].items():
    digest = hashlib.sha256()
    count, size, max_length, clustered, remaining = 0, 0, 0, 0, b""
    separator = b"\n" if name.endswith(".txt") else b"\0"
    with (args.corpus / name).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
            size += len(block)
            rows = (remaining + block).split(separator)
            remaining = rows.pop()
            count += len(rows)
            max_length = max(max_length, max(map(len, rows), default=0))
            clustered += sum(key.startswith(b"com.") for key in rows)
    assert not remaining and digest.hexdigest() == expected["sha256"] and size == expected["bytes"], name
    assert count == (metadata["arrival_keys"] if name == "arrivals.nul" else metadata["base_keys"])
    assert max_length <= 254 and clustered > 0
    profile[name] = {"sha256": digest.hexdigest(), "keys": count, "bytes": size,
                     "max_key_bytes": max_length, "com_prefix_keys": clustered}
dependencies = {}
for name in ("measured_art_hot_dependencies.d", "measured_lits_dependencies.d"):
    dependency_text = (args.records / name).read_text().replace("\\\n", " ")
    paths = dependency_text.split(":", 1)[1].split()
    assert not any("/LITS/" in path for path in paths) if "art_hot" in name else not any("/hot/libs/" in path for path in paths)
    dependencies[name] = {path: hashlib.sha256(Path(path).read_bytes()).hexdigest() for path in paths}
record = {"preparation": metadata, "verified_profile": profile, "dependencies_sha256": dependencies,
          "upstream_hot_and_lits_fork_compiled_separately": True,
          "no_natural_key_truncation_or_exclusion": True,
          "sampling_scope": "systematic across 200M selection, not random across full 240M release"}
with (args.records / "verified_inputs.json").open("x") as stream:
    json.dump(record, stream, indent=2)
print(json.dumps(profile), flush=True)
