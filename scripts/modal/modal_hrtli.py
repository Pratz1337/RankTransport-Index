"""Modal jobs for the 2026-10-23 equal-contract study.

Heavy work (corpus reconstruction, native builds, timed runs) executes in Modal
containers so that it does not compete with the 16-GiB laptop host. Every job
writes into the persistent volume ``hrtli-data`` and returns JSON records that
the caller stores under results/replicated_rankcap/.

Usage (from the repository root):
    python -m modal run scripts/modal/modal_hrtli.py::run_prepare_commoncrawl
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import modal

ROOT = Path(__file__).resolve().parents[2]
app = modal.App("hrtli-equal-contract-20261023")
volume = modal.Volume.from_name("hrtli-data", create_if_missing=True)

image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install("g++", "make", "time", "procps", "util-linux", "zstd", "p7zip-full", "wget")
    .add_local_file(ROOT / "scripts/prepare_commoncrawl_200m.py", "/work/scripts/prepare_commoncrawl_200m.py")
    .add_local_file(ROOT / "scripts/prepare_lits_scale_corpora.py", "/work/scripts/prepare_lits_scale_corpora.py")
    .add_local_file(ROOT / "datasets/commoncrawl_200m/commoncrawl_hosts_200m_metadata.json",
                    "/work/meta/cc200m_metadata.json")
    .add_local_file(ROOT / "results/adaptive_competitors_20260906/verified_inputs.json",
                    "/work/meta/verified_10m_inputs.json")
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(url: str, target: Path, expected_sha256: str | None, attempts: int = 8) -> dict:
    for attempt in range(attempts):
        try:
            if not target.exists() or (expected_sha256 and sha256_file(target) != expected_sha256):
                tmp = target.with_suffix(target.suffix + ".part")
                request = urllib.request.Request(url, headers={"User-Agent": "hrtli-research/1.0"})
                with urllib.request.urlopen(request, timeout=120) as response, tmp.open("wb") as out:
                    while chunk := response.read(8 << 20):
                        out.write(chunk)
                tmp.rename(target)
            digest = sha256_file(target)
            if expected_sha256 and digest != expected_sha256:
                target.unlink()
                raise RuntimeError(f"sha256 mismatch for {url}: {digest}")
            return {"url": url, "bytes": target.stat().st_size, "sha256": digest}
        except Exception as error:  # noqa: BLE001 - retried and finally re-raised
            if attempt + 1 == attempts:
                raise
            time.sleep(min(120, 5 * 2 ** attempt))
            print(f"retry {attempt + 1} {url}: {error}", flush=True)
    raise AssertionError("unreachable")


@app.function(image=image, volumes={"/data": volume}, cpu=8, memory=16384, timeout=4 * 3600)
def prepare_commoncrawl() -> dict:
    """Rebuild the verified 200M split and its 1M/10M systematic samples."""
    metadata = json.loads(Path("/work/meta/cc200m_metadata.json").read_text())
    expected10 = json.loads(Path("/work/meta/verified_10m_inputs.json").read_text())["preparation"]["files"]
    shard_dir = Path("/data/cc/shards")
    shard_dir.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=6) as pool:
        fetched = list(pool.map(lambda row: fetch(row["url"], shard_dir / row["file"], row["sha256"]),
                                metadata["shards"]))
    volume.commit()
    prepared = Path("/data/cc/prepared")
    if not (prepared / "commoncrawl_hosts_200m_metadata.json").exists():
        subprocess.run(["python", "/work/scripts/prepare_commoncrawl_200m.py", "--shard-dir", str(shard_dir),
                        "--output-dir", str(prepared)], check=True)
    regenerated = json.loads((prepared / "commoncrawl_hosts_200m_metadata.json").read_text())
    for field in ("initial_sha256", "insert_sha256", "release_stream_sha256", "release_records"):
        assert regenerated[field] == metadata[field], (field, regenerated[field], metadata[field])
    volume.commit()
    samples = Path("/data/cc/samples")
    if not samples.exists():
        subprocess.run(["python", "/work/scripts/prepare_lits_scale_corpora.py", str(prepared), str(samples)],
                       check=True)
    provenance10 = json.loads((samples / "sample_10000000/provenance.json").read_text())
    for name, row in expected10.items():
        assert provenance10["files"][name]["sha256"] == row["sha256"], (name, provenance10["files"][name])
    volume.commit()
    return {"shards": fetched, "regenerated_200m_metadata": regenerated,
            "sample_10m_provenance": provenance10,
            "sample_1m_provenance": json.loads((samples / "sample_1000000/provenance.json").read_text())}


@app.function(image=image, volumes={"/data": volume}, cpu=4, memory=32768, timeout=3 * 3600)
def prepare_corpus(name: str, sources: list[dict], key_rule: str, max_keys: int = 0, hash_modulus: int = 1) -> dict:
    """Generic natural-key corpus: verified downloads -> bytewise sort/unique -> 4:1 split.

    ``sources`` rows: {"url": ..., "path": volume path, "sha256": optional}.  ``key_rule``
    names a fixed extraction rule (no generated keys).  Every fifth key of the sorted
    unique set (one-indexed) is held out as an arrival, mirroring the Common Crawl samples.
    If ``max_keys`` is set and smaller than the unique count, a systematic stride over
    the sorted unique set selects the sample before the split.
    """
    import gzip
    import lzma
    import re

    sql_row = re.compile(rb"\((\d+),(\d+),'((?:[^'\\]|\\.)*)','((?:[^'\\]|\\.)*)'\)")
    sql_escapes = {b"0": b"\0", b"n": b"\n", b"r": b"\r", b"t": b"\t", b"Z": b"\x1a"}

    def sql_unescape(raw: bytes) -> bytes:
        if b"\\" not in raw:
            return raw
        out, i = bytearray(), 0
        while i < len(raw):
            c = raw[i:i + 1]
            if c == b"\\" and i + 1 < len(raw):
                nxt = raw[i + 1:i + 2]
                out += sql_escapes.get(nxt, nxt)
                i += 2
            else:
                out += c
                i += 1
        return bytes(out)

    def sampled(key: bytes) -> bool:
        if hash_modulus <= 1:
            return True
        return int.from_bytes(hashlib.blake2b(key, digest_size=8).digest(), "little") % hash_modulus == 0

    fetched = []
    for row in sources:
        target = Path(row["path"])
        target.parent.mkdir(parents=True, exist_ok=True)
        if row.get("url"):
            fetched.append(fetch(row["url"], target, row.get("sha256")))
        else:
            digest = sha256_file(target)
            assert not row.get("sha256") or digest == row["sha256"], (target, digest)
            fetched.append({"path": str(target), "bytes": target.stat().st_size, "sha256": digest})
    volume.commit()

    def lines(path: Path):
        opener = gzip.open if path.suffix == ".gz" else lzma.open if path.suffix == ".xz" else open
        with opener(path, "rb") as handle:
            for raw in handle:
                yield raw.rstrip(b"\n")

    keys: set[bytes] = set()
    raw_records = rejected = 0
    for row in sources:
        if key_rule == "enwiki_externallinks":
            # MediaWiki >= 1.40 schema: (el_id, el_from, el_to_domain_index, el_to_path);
            # the natural key is the reversed-host domain index followed by the path.
            for line in lines(Path(row["path"])):
                if not line.startswith(b"INSERT INTO"):
                    continue
                for match in sql_row.finditer(line):
                    raw_records += 1
                    key = sql_unescape(match.group(3)) + sql_unescape(match.group(4))
                    if not key or b"\0" in key or b"\r" in key or b"\n" in key:
                        rejected += 1
                        continue
                    if sampled(key):
                        keys.add(key)
            continue
        for line in lines(Path(row["path"])):
            raw_records += 1
            if key_rule == "enwiki_titles":
                if line == b"page_title":
                    continue
                key = line
            elif key_rule == "debian_contents_paths":
                # "<path><whitespace><section/package,...>"; header-free since Debian 2017.
                key = line.rsplit(None, 1)[0] if b" " in line or b"\t" in line else b""
                key = b"/" + key.rstrip() if key and not key.startswith(b"/") else key.rstrip()
            elif key_rule == "plain_lines":
                key = line
            else:
                raise ValueError(key_rule)
            if not key or b"\0" in key or b"\r" in key:
                rejected += 1
                continue
            if sampled(key):
                keys.add(key)
    ordered = sorted(keys)
    unique = len(ordered)
    del keys
    stride = 1
    if max_keys and unique > max_keys:
        stride = -(-unique // max_keys)
        ordered = ordered[::stride]
    out = Path(f"/data/corpora/{name}")
    out.mkdir(parents=True, exist_ok=True)
    digests = {n: hashlib.sha256() for n in ("base.txt", "arrivals.nul")}
    counts = {"base": 0, "arrivals": 0}
    lengths = []
    with (out / "base.txt").open("wb") as base, (out / "arrivals.nul").open("wb") as arrivals:
        for index, key in enumerate(ordered):
            lengths.append(len(key))
            if (index + 1) % 5 == 0:
                arrivals.write(key + b"\0")
                digests["arrivals.nul"].update(key + b"\0")
                counts["arrivals"] += 1
            else:
                base.write(key + b"\n")
                digests["base.txt"].update(key + b"\n")
                counts["base"] += 1
    lengths.sort()
    record = {
        "name": name, "key_rule": key_rule, "sources": fetched, "raw_records": raw_records,
        "rejected_records": rejected, "unique_keys": unique, "systematic_stride": stride,
        "hash_sampling": {"function": "blake2b-64 little-endian mod m == 0", "modulus": hash_modulus},
        "selected_keys": len(ordered), "base_keys": counts["base"], "arrival_keys": counts["arrivals"],
        "split": "sorted unique keys; every fifth (one-indexed) held out as an arrival",
        "natural_keys_not_generated": True,
        "key_bytes": {"mean": sum(lengths) / len(lengths), "median": lengths[len(lengths) // 2],
                      "p99": lengths[int(len(lengths) * 0.99)], "max": lengths[-1]},
        "files": {n: {"sha256": d.hexdigest(), "bytes": (out / n).stat().st_size} for n, d in digests.items()},
    }
    (out / "provenance.json").write_text(json.dumps(record, indent=2) + "\n")
    volume.commit()
    return record


CXXFLAGS = ["-std=c++17", "-O3", "-DNDEBUG", "-march=x86-64-v3", "-DHRTLI_WITH_COUNTED_ART", "-DHRTLI_WITH_FENCED"]
bench_image = image.add_local_dir(ROOT / "hrtli_cpp", "/work/hrtli_cpp",
                                  ignore=lambda p: not str(p).endswith((".hpp", ".cpp", ".inc")))


def telemetry() -> dict:
    meminfo = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines() if ":" in line)
    cpu = [line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
           if line.startswith("model name")]
    vm = {}
    try:
        vm = {k: int(v) for k, v in (line.split() for line in Path("/proc/vmstat").read_text().splitlines())
              if k in ("pswpin", "pswpout", "pgmajfault")}
    except OSError:
        pass
    fields = {}
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if ":" in line:
            k, v = (x.strip() for x in line.split(":", 1))
            if k in ("vendor_id", "cpu family", "model", "stepping", "cpu MHz", "cache size") and k not in fields:
                fields[k] = v
            if k == "flags" and "flags_sha256" not in fields:
                fields["flags_sha256"] = hashlib.sha256(v.encode()).hexdigest()[:16]
                fields["avx2"] = "avx2" in v.split()
    return {"unix_time": time.time(), "cpu_model": cpu[0] if cpu else None, "cpu_fields": fields,
            "logical_cpus": os.cpu_count(),
            "mem_available_kib": meminfo.get("MemAvailable", "").strip(),
            "mem_total_kib": meminfo.get("MemTotal", "").strip(), "vmstat": vm,
            "loadavg": Path("/proc/loadavg").read_text().strip()}


def build_driver(source: str, output: str, extra: list[str] | None = None) -> dict:
    flags = CXXFLAGS + (extra or [])
    subprocess.run(["g++", *flags, "-o", output, f"/work/hrtli_cpp/{source}"], check=True)
    sources = {p.name: sha256_file(p) for p in sorted(Path("/work/hrtli_cpp").glob("*.[hc]pp"))
               if p.name in {source, "packed_rank_transport.hpp", "prefix_radix_delta.hpp", "approx_local_delta.hpp",
                             "counted_radix_ost.hpp", "counted_art.hpp", "fenced_rank_transport.hpp"}}
    compiler = subprocess.run(["g++", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    return {"source": source, "flags": flags, "compiler": compiler, "binary_sha256": sha256_file(Path(output)),
            "source_sha256": sources}


CORPORA = {
    "cc_hosts_10m": ("/data/cc/samples/sample_10000000/base.txt", "/data/cc/samples/sample_10000000/arrivals.nul",
                     8_000_000, 2_000_000),
    "cc_hosts_1m": ("/data/cc/samples/sample_1000000/base.txt", "/data/cc/samples/sample_1000000/arrivals.nul",
                    800_000, 200_000),
}


def resolve_corpus(corpus: str) -> tuple[str, str, int, int]:
    if corpus in CORPORA:
        return CORPORA[corpus]
    root = Path(f"/data/corpora/{corpus}")
    record = json.loads((root / "provenance.json").read_text())
    return str(root / "base.txt"), str(root / "arrivals.nul"), record["base_keys"], record["arrival_keys"]


@app.function(image=bench_image, volumes={"/data": volume}, cpu=4, memory=32768, timeout=6 * 3600)
def equal_contract_session(corpus: str, delta: int, queries: int, seed: int, epsilon: int,
                           modes: list[str], extra_flags: list[str] | None = None,
                           workload: str = "standard") -> dict:
    """One machine, one trace, every method in a fresh process, in the given order."""
    base, arrivals, base_n, arrival_n = resolve_corpus(corpus)
    build = build_driver("benchmark_equal_contract.cpp", "/tmp/equal_contract", extra_flags)
    inputs = {path: sha256_file(Path(path)) for path in (base, arrivals)}
    runs = []
    for mode in modes:
        before = telemetry()
        started = time.time()
        proc = subprocess.run(["/tmp/equal_contract", mode, base, arrivals, str(base_n), str(arrival_n), str(delta),
                               str(queries), str(seed), str(epsilon), workload], capture_output=True, text=True)
        runs.append({"mode": mode, "exit_code": proc.returncode, "wall_seconds": time.time() - started,
                     "telemetry_before": before, "telemetry_after": telemetry(),
                     "stdout": proc.stdout, "stderr": proc.stderr[-4000:],
                     "result": json.loads(proc.stdout) if proc.returncode == 0 and proc.stdout.strip() else None})
        print(mode, proc.returncode, proc.stderr[-300:], flush=True)
    return {"corpus": corpus, "delta": delta, "queries": queries, "seed": seed, "epsilon": epsilon,
            "workload": workload, "modes_in_order": modes, "build": build, "inputs_sha256": inputs, "runs": runs}


RECORDS = ROOT / "results/replicated_rankcap"


def save_record(name: str, record: dict) -> None:
    RECORDS.mkdir(parents=True, exist_ok=True)
    (RECORDS / name).write_text(json.dumps(record, indent=2) + "\n")
    print(f"wrote {RECORDS / name}")


@app.local_entrypoint()
def run_prepare_commoncrawl() -> None:
    save_record("corpus_commoncrawl.json", prepare_commoncrawl.remote())


@app.local_entrypoint()
def run_pilot(corpus: str = "cc_hosts_10m", delta: int = 80_000, queries: int = 1_048_576,
              seed: int = 20261023, epsilon: int = 64,
              modes: str = "profile,hrtli,hrtli_binary,counted_art,radix_ost,pbds_rbtree",
              label: str = "pilot", workload: str = "standard") -> None:
    """Exploratory (development) session; labelled as such in its record name."""
    record = equal_contract_session.remote(corpus, delta, queries, seed, epsilon, modes.split(","),
                                           None, workload)
    record["label"] = label
    tag = "" if workload == "standard" else "_" + workload.replace(":", "-")
    save_record(f"{label}_{corpus}_d{delta}_s{seed}_e{epsilon}{tag}.json", record)


@app.local_entrypoint()
def run_prepare_enwiki() -> None:
    record = prepare_corpus.remote("enwiki_titles", [{
        "path": "/data/raw/enwiki-20260811-all-titles-in-ns-0.gz",
        "url": "https://dumps.wikimedia.org/other/pagetitles/20260811/enwiki-20260811-all-titles-in-ns-0.gz",
        "sha256": "cf23b16dc7e4091f9d13d2c2fb039de17f7979252cea4c7ec39b1d82cf41bdb8"}], "enwiki_titles")
    save_record("corpus_enwiki_titles.json", record)


@app.local_entrypoint()
def run_prepare_paths() -> None:
    record = prepare_corpus.remote("ubuntu_noble_paths", [{
        "path": "/data/raw/ubuntu-noble-Contents-amd64.gz",
        "url": "https://archive.ubuntu.com/ubuntu/dists/noble/Contents-amd64.gz"}], "debian_contents_paths")
    save_record("corpus_ubuntu_noble_paths.json", record)


@app.local_entrypoint()
def run_prepare_urls(modulus: int = 10) -> None:
    record = prepare_corpus.remote("enwiki_externallinks", [{
        "path": "/data/raw/enwiki-20261001-externallinks.sql.gz",
        "url": "https://dumps.wikimedia.org/enwiki/20261001/enwiki-20261001-externallinks.sql.gz"}],
        "enwiki_externallinks", hash_modulus=modulus)
    save_record("corpus_enwiki_externallinks.json", record)
