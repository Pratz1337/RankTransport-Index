"""Build and audit the 200M-key Common Crawl host benchmark corpus."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path


RELEASE = "cc-main-2026-may-jun-jul"
EXPECTED_RELEASE_RECORDS = 240_380_987
SOURCE_PREFIX = f"https://data.commoncrawl.org/projects/hyperlinkgraph/{RELEASE}/host/vertices"


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shard-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--initial-count", type=int, default=199_900_000)
    parser.add_argument("--insert-count", type=int, default=100_000)
    args = parser.parse_args()

    selected_count = args.initial_count + args.insert_count
    if selected_count < 200_000_000:
        raise SystemExit("the selected real corpus must contain at least 200M keys")
    if selected_count % args.insert_count != 0:
        raise SystemExit("selected count must be divisible by insert count")
    insert_stride = selected_count // args.insert_count

    shards = sorted(args.shard_dir.glob("part-*.txt.gz"))
    if len(shards) != 48:
        raise SystemExit(f"expected 48 official vertex shards, found {len(shards)}")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    initial_path = args.output_dir / "commoncrawl_hosts_200m_initial.txt"
    insert_path = args.output_dir / "commoncrawl_hosts_200m_insert.txt"
    metadata_path = args.output_dir / "commoncrawl_hosts_200m_metadata.json"

    initial_digest = hashlib.sha256()
    insert_digest = hashlib.sha256()
    release_digest = hashlib.sha256()
    record_count = 0
    initial_written = 0
    inserts_written = 0
    previous_key: bytes | None = None
    first_key = last_selected_key = last_release_key = None
    shard_rows: list[dict[str, object]] = []

    with initial_path.open("wb") as initial_out, insert_path.open("wb") as insert_out:
        for shard in shards:
            shard_records = 0
            shard_first_id = None
            shard_last_id = None
            with gzip.open(shard, "rb") as source:
                for raw in source:
                    release_digest.update(raw)
                    fields = raw.rstrip(b"\n").split(b"\t", 1)
                    if len(fields) != 2:
                        raise SystemExit(f"malformed record in {shard}: {raw[:120]!r}")
                    host_id = int(fields[0])
                    key = fields[1]
                    if host_id != record_count:
                        raise SystemExit(
                            f"non-contiguous host ID: expected {record_count}, got {host_id}"
                        )
                    if previous_key is not None and previous_key >= key:
                        raise SystemExit(
                            f"keys are not strictly bytewise sorted at host ID {host_id}"
                        )
                    if first_key is None:
                        first_key = key.decode("utf-8")
                    previous_key = key
                    last_release_key = key.decode("utf-8")
                    line = key + b"\n"
                    if record_count < selected_count:
                        if (record_count + 1) % insert_stride == 0:
                            insert_out.write(line)
                            insert_digest.update(line)
                            inserts_written += 1
                        else:
                            initial_out.write(line)
                            initial_digest.update(line)
                            initial_written += 1
                        last_selected_key = key.decode("utf-8")
                    record_count += 1
                    shard_records += 1
                    if shard_first_id is None:
                        shard_first_id = host_id
                    shard_last_id = host_id
            shard_rows.append(
                {
                    "file": shard.name,
                    "url": f"{SOURCE_PREFIX}/{shard.name}",
                    "compressed_bytes": shard.stat().st_size,
                    "sha256": digest_file(shard),
                    "records": shard_records,
                    "first_host_id": shard_first_id,
                    "last_host_id": shard_last_id,
                }
            )

    if record_count != EXPECTED_RELEASE_RECORDS:
        raise SystemExit(
            f"release count mismatch: expected {EXPECTED_RELEASE_RECORDS}, got {record_count}"
        )
    if initial_written != args.initial_count or inserts_written != args.insert_count:
        raise SystemExit(
            f"split mismatch: initial={initial_written}, inserts={inserts_written}"
        )

    metadata = {
        "dataset": "Common Crawl host-level Web Graph May-June-July 2026",
        "release": RELEASE,
        "official_announcement": (
            "https://commoncrawl.org/blog/"
            "host--and-domain-level-web-graphs-may-june-and-july-2026"
        ),
        "accessed": "2026-08-27",
        "source_representation": "official <host ID, reverse-domain host> vertex records",
        "key_order": "strict ascending raw UTF-8 byte order, verified over the full release",
        "selection": (
            f"contiguous official host IDs 0 through {selected_count - 1}; "
            f"every {insert_stride}th selected key is held out for insertion, "
            f"leaving {args.initial_count} frozen-base keys"
        ),
        "insert_stride": insert_stride,
        "release_records": record_count,
        "selected_records": selected_count,
        "initial_keys": args.initial_count,
        "insert_keys": args.insert_count,
        "first_key": first_key,
        "last_selected_key": last_selected_key,
        "last_release_key": last_release_key,
        "release_stream_sha256": release_digest.hexdigest(),
        "initial_sha256": initial_digest.hexdigest(),
        "insert_sha256": insert_digest.hexdigest(),
        "initial_bytes": initial_path.stat().st_size,
        "insert_bytes": insert_path.stat().st_size,
        "shards": shard_rows,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(
        f"PASS release_records={record_count} selected_records={selected_count} "
        f"initial={args.initial_count} insert={args.insert_count}"
    )


if __name__ == "__main__":
    main()
