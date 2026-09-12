"""Download and derive redistributable benchmark corpora.

The generated HRT-LI keys are derived from public datasets/specification test
fixtures with explicit redistribution licenses:

- Public Suffix List, MPL-2.0, for DNS hierarchy keys.
- Web Platform Tests URL fixtures, BSD-3-Clause, for URL and JSON-path keys.
- package-url specification tests, MIT, for package-manager hierarchy keys.
"""

from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from urllib.parse import quote, unquote, urlparse
from urllib.request import Request, urlopen


PUBLIC_SUFFIX_URL = "https://publicsuffix.org/list/public_suffix_list.dat"
WPT_URLTESTDATA_URL = (
    "https://raw.githubusercontent.com/web-platform-tests/wpt/master/url/resources/urltestdata.json"
)
WPT_LICENSE_URL = "https://raw.githubusercontent.com/web-platform-tests/wpt/master/LICENSE.md"
PURL_TREE_URL = "https://api.github.com/repos/package-url/purl-spec/git/trees/main?recursive=1"
PURL_RAW_PREFIX = "https://raw.githubusercontent.com/package-url/purl-spec/main/"
PURL_LICENSE_URL = "https://raw.githubusercontent.com/package-url/purl-spec/main/LICENSE"


def fetch_text(url: str) -> str:
    request = Request(url, headers={"User-Agent": "hrtli-corpus-builder/1.0"})
    with urlopen(request, timeout=30) as response:
        return response.read().decode("utf-8", errors="replace")


def write_split(name: str, paths: list[str], data_dir: Path, initial_ratio: float = 0.8) -> None:
    data_dir.mkdir(parents=True, exist_ok=True)
    split = int(len(paths) * initial_ratio)
    (data_dir / f"{name}_initial.txt").write_text("\n".join(paths[:split]) + "\n", encoding="utf-8")
    (data_dir / f"{name}_insert.txt").write_text("\n".join(paths[split:]) + "\n", encoding="utf-8")


def build_publishable_corpora(max_paths: int = 10000, source_dir: str | Path = "data_sources") -> dict[str, list[str]]:
    source_path = Path(source_dir)
    raw_dir = source_path / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    psl_text = _cached_fetch(raw_dir / "public_suffix_list.dat", PUBLIC_SUFFIX_URL)
    wpt_text = _cached_fetch(raw_dir / "wpt_urltestdata.json", WPT_URLTESTDATA_URL)
    purl_tree_text = _cached_fetch(raw_dir / "purl_tree.json", PURL_TREE_URL)
    _cached_fetch(raw_dir / "wpt_LICENSE.md", WPT_LICENSE_URL)
    _cached_fetch(raw_dir / "purl_LICENSE", PURL_LICENSE_URL)

    wpt_payload = json.loads(wpt_text)
    purl_payloads = _fetch_purl_payloads(json.loads(purl_tree_text), raw_dir)

    corpora = {
        "url": _limit(_url_paths_from_wpt(wpt_payload), max_paths),
        "dns": _limit(_dns_paths_from_psl(psl_text), max_paths),
        "json": _limit(_json_paths_from_payload(wpt_payload), max_paths),
        "package": _limit(_package_paths_from_purl_tests(purl_payloads), max_paths),
    }
    _write_sources_manifest(source_path, corpora)
    return corpora


def export_publishable_corpora(
    data_dir: str | Path = "data",
    source_dir: str | Path = "data_sources",
    max_paths: int = 10000,
    initial_ratio: float = 0.8,
) -> None:
    corpora = build_publishable_corpora(max_paths=max_paths, source_dir=source_dir)
    out_dir = Path(data_dir)
    for name, paths in corpora.items():
        write_split(name, paths, out_dir, initial_ratio=initial_ratio)
        print(f"Exported publishable {name}: {int(len(paths) * initial_ratio)} initial, {len(paths) - int(len(paths) * initial_ratio)} insert keys.")


def _cached_fetch(path: Path, url: str) -> str:
    if path.exists() and path.stat().st_size > 0:
        return path.read_text(encoding="utf-8", errors="replace")
    text = fetch_text(url)
    path.write_text(text, encoding="utf-8")
    return text


def _limit(paths: list[str], max_paths: int) -> list[str]:
    return sorted(set(paths))[:max_paths]


def _segment(value: str) -> str:
    cleaned = "".join(ch if ch >= " " and ch != "/" else "_" for ch in str(value).strip())
    return quote(cleaned or "_", safe="._-~@:%")


def _url_paths_from_wpt(payload: list[Any]) -> list[str]:
    paths: list[str] = []
    for entry in payload:
        if not isinstance(entry, dict):
            continue
        for field in ("href", "base", "input"):
            value = entry.get(field)
            if isinstance(value, str):
                key = _normalise_url(value)
                if key is not None:
                    paths.append(key)
    return paths


def _normalise_url(raw_url: str) -> str | None:
    try:
        parsed = urlparse(raw_url)
    except ValueError:
        return None
    if parsed.scheme not in {"http", "https", "ftp", "file"}:
        return None
    host = parsed.hostname or parsed.netloc
    if not host:
        return None
    host_parts = [_segment(unquote(part.lower())) for part in reversed(host.split(".")) if part]
    path_parts = [_segment(unquote(part)) for part in parsed.path.split("/") if part]
    key_parts = ["url", _segment(parsed.scheme), *host_parts, *path_parts]
    if parsed.query:
        query_keys = [part.split("=", 1)[0] for part in parsed.query.split("&") if part]
        key_parts.extend(["query", *[_segment(unquote(key)) for key in sorted(query_keys)[:8]]])
    return "/" + "/".join(key_parts)


def _dns_paths_from_psl(psl_text: str) -> list[str]:
    paths: list[str] = []
    for line in psl_text.splitlines():
        key = _normalise_dns_rule(line)
        if key is not None:
            paths.append(key)
    return paths


def _normalise_dns_rule(line: str) -> str | None:
    rule = line.strip()
    if not rule or rule.startswith("//"):
        return None
    if rule.startswith("!"):
        rule = rule[1:]
    if rule.startswith("*."):
        rule = rule[2:]
    labels = [_segment(label.lower()) for label in rule.strip(".").split(".") if label]
    if not labels:
        return None
    return "/dns/" + "/".join(reversed(labels))


def _json_paths_from_payload(payload: Any) -> list[str]:
    paths: list[str] = []
    _collect_json_paths(payload, "/json/wpt_urltestdata", paths)
    return paths


def _collect_json_paths(value: Any, prefix: str, out: list[str]) -> None:
    out.append(prefix)
    if isinstance(value, dict):
        for key in sorted(value):
            _collect_json_paths(value[key], f"{prefix}/{_segment(str(key))}", out)
    elif isinstance(value, list):
        width = max(4, len(str(len(value))))
        for idx, item in enumerate(value):
            _collect_json_paths(item, f"{prefix}/[{idx:0{width}d}]", out)


def _fetch_purl_payloads(tree: dict[str, Any], raw_dir: Path) -> list[dict[str, Any]]:
    payloads: list[dict[str, Any]] = []
    paths = [
        item["path"]
        for item in tree.get("tree", [])
        if isinstance(item, dict)
        and item.get("type") == "blob"
        and (
            re.fullmatch(r"tests/(spec|types)/.+-test\.json", item.get("path", ""))
            or re.fullmatch(r"types/.+-definition\.json", item.get("path", ""))
        )
    ]
    cache_dir = raw_dir / "purl_payloads"
    cache_dir.mkdir(parents=True, exist_ok=True)
    for path in sorted(paths):
        cache_name = path.replace("/", "__")
        text = _cached_fetch(cache_dir / cache_name, PURL_RAW_PREFIX + path)
        payloads.append(json.loads(text))
    return payloads


def _package_paths_from_purl_tests(payloads: list[dict[str, Any]]) -> list[str]:
    paths: list[str] = []
    for payload in payloads:
        for example in payload.get("examples", []):
            if isinstance(example, str) and example.startswith("pkg:"):
                key = _normalise_purl(example)
                if key is not None:
                    paths.append(key)
        for test in payload.get("tests", []):
            if not isinstance(test, dict):
                continue
            if test.get("expected_failure") is True:
                continue
            values = [test.get("input"), test.get("expected_output")]
            for value in values:
                if isinstance(value, str) and value.startswith("pkg:"):
                    key = _normalise_purl(value)
                    if key is not None:
                        paths.append(key)
                elif isinstance(value, dict):
                    key = _normalise_purl_fields(value)
                    if key is not None:
                        paths.append(key)
    return paths


def _normalise_purl(purl: str) -> str | None:
    if not purl.startswith("pkg:"):
        return None
    body = purl[4:]
    body, _, subpath = body.partition("#")
    body, _, qualifiers = body.partition("?")
    if "/" not in body:
        return None
    purl_type, name_part = body.split("/", 1)
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+-]*", purl_type) is None:
        return None
    name_part, _, version = name_part.partition("@")
    parts = ["pkg", _segment(unquote(purl_type).lower())]
    parts.extend(_segment(unquote(part)) for part in name_part.split("/") if part)
    if version:
        parts.extend(["version", _segment(unquote(version))])
    if qualifiers:
        qualifier_keys = [item.split("=", 1)[0] for item in qualifiers.split("&") if item]
        parts.extend(["qualifier", *[_segment(unquote(key)) for key in sorted(qualifier_keys)[:8]]])
    if subpath:
        parts.extend(["subpath", *[_segment(unquote(part)) for part in subpath.split("/") if part]])
    return "/" + "/".join(parts)


def _normalise_purl_fields(fields: dict[str, Any]) -> str | None:
    purl_type = fields.get("type")
    name = fields.get("name")
    if not isinstance(purl_type, str) or not isinstance(name, str):
        return None
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+-]*", purl_type) is None:
        return None
    parts = ["pkg", _segment(purl_type.lower())]
    namespace = fields.get("namespace")
    if isinstance(namespace, str) and namespace:
        parts.extend(_segment(part) for part in namespace.split("/") if part)
    parts.append(_segment(name))
    version = fields.get("version")
    if isinstance(version, str) and version:
        parts.extend(["version", _segment(version)])
    qualifiers = fields.get("qualifiers")
    if isinstance(qualifiers, dict) and qualifiers:
        parts.extend(["qualifier", *[_segment(str(key)) for key in sorted(qualifiers)[:8]]])
    subpath = fields.get("subpath")
    if isinstance(subpath, str) and subpath:
        parts.extend(["subpath", *[_segment(part) for part in subpath.split("/") if part]])
    return "/" + "/".join(parts)


def _write_sources_manifest(source_dir: Path, corpora: dict[str, list[str]]) -> None:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        "# Publishable Corpus Sources",
        "",
        f"Generated: {generated}",
        "",
        "| Corpus | Source | License | Derived keys |",
        "|---|---|---|---:|",
        f"| DNS | {PUBLIC_SUFFIX_URL} | MPL-2.0 | {len(corpora['dns'])} |",
        f"| URL | {WPT_URLTESTDATA_URL} | BSD-3-Clause | {len(corpora['url'])} |",
        f"| JSON paths | {WPT_URLTESTDATA_URL} | BSD-3-Clause | {len(corpora['json'])} |",
        f"| Package manager | package-url tests and type definitions under {PURL_RAW_PREFIX} | MIT | {len(corpora['package'])} |",
        "",
        "The `data/*_initial.txt` and `data/*_insert.txt` files are normalized hierarchical keys derived from these sources.",
        "Keep the raw source files and this manifest with benchmark artifacts so paper reviewers can reproduce the exact corpus build.",
    ]
    (source_dir / "SOURCES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    export_publishable_corpora()


if __name__ == "__main__":
    main()
