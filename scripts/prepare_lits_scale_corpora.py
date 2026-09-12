"""Systematic natural-key samples across the verified 200M selection, not prefixes."""
import argparse
import hashlib
import json
from contextlib import ExitStack
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
metadata_path = args.source / "commoncrawl_hosts_200m_metadata.json"
metadata = json.loads(metadata_path.read_text())
assert metadata["selected_records"] == 200000000 and metadata["insert_stride"] == 2000
args.destination.mkdir()  # Refuse to merge with any pre-existing preparation.
with ExitStack() as stack:
    source_paths = [args.source / f"commoncrawl_hosts_200m_{suffix}.txt" for suffix in ("initial", "insert")]
    sources = [stack.enter_context(path.open("rb")) for path in source_paths]
    source_hashes = [hashlib.sha256(), hashlib.sha256()]
    samples = []
    for stride in (200, 20):
        directory = args.destination / f"sample_{200000000 // stride}"
        directory.mkdir()
        names = ("base.txt", "base.nul", "arrivals.nul")
        streams = [stack.enter_context((directory / name).open("xb")) for name in names]
        samples.append({"stride": stride, "directory": directory, "streams": streams,
                        "hashes": [hashlib.sha256() for _ in names], "count": 0,
                        "base": 0, "arrivals": 0, "names": names})
    previous = None
    for source_id in range(metadata["selected_records"]):
        which = int((source_id + 1) % 2000 == 0)
        line = sources[which].readline()
        assert line.endswith(b"\n"), (source_id, "missing natural source record")
        source_hashes[which].update(line)
        key = line[:-1]
        assert previous is None or previous < key, (source_id, "nonascending natural source")
        previous = key
        for sample in samples:
            if source_id % sample["stride"]:
                continue
            assert key and b"\0" not in key and b"\r" not in key and all(c < 128 for c in key)
            held_out = sample["count"] % 5 == 0
            nul = key + b"\0"
            outputs = ((2, nul),) if held_out else ((0, line), (1, nul))
            for index, data in outputs:
                sample["streams"][index].write(data)
                sample["hashes"][index].update(data)
            sample["count"] += 1
            sample["arrivals" if held_out else "base"] += 1
        if source_id % 20000000 == 19999999:
            print(f"verified_source_keys={source_id + 1}", flush=True)
    assert all(stream.read(1) == b"" for stream in sources), "unexpected source tail"
    assert source_hashes[0].hexdigest() == metadata["initial_sha256"]
    assert source_hashes[1].hexdigest() == metadata["insert_sha256"]
    for sample in samples:
        for stream in sample["streams"]:
            stream.flush()
        record = {
            "release": metadata["release"], "parent_selected_keys": metadata["selected_records"],
            "parent_metadata_sha256": hashlib.sha256(metadata_path.read_bytes()).hexdigest(),
            "parent_sources_sha256": {str(path): digest.hexdigest() for path, digest in zip(source_paths, source_hashes)},
            "selection": "original source IDs divisible by sample_stride; every fifth sampled key held out",
            "sample_stride": sample["stride"], "sample_keys": sample["count"],
            "base_keys": sample["base"], "arrival_keys": sample["arrivals"],
            "natural_keys_not_generated": True, "full_parent_order_and_hashes_verified": True,
            "files": {name: {"sha256": digest.hexdigest(), "bytes": (sample["directory"] / name).stat().st_size}
                      for name, digest in zip(sample["names"], sample["hashes"])},
        }
        (sample["directory"] / "provenance.json").write_text(json.dumps(record, indent=2) + "\n")
        print(json.dumps(record), flush=True)
