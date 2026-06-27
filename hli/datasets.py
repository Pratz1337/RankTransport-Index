"""
Real-world and large-scale hierarchical dataset loaders for HRT-LI benchmarks.

Provides:
  1. Linux kernel file paths (scraped from git ls-tree)
  2. Synthetic DNS zone records (realistic hierarchy)
  3. Synthetic JSON document paths (MongoDB-style nested documents)
  4. Scalable synthetic tree generator (10K - 1M nodes)
  5. Wikipedia category path loader (real-world hierarchical data)
  6. SOSD-style integer key wrapper for fair comparison
  
All datasets return a standardized format: list of path strings, sorted lexicographically.
"""

import importlib.metadata as importlib_metadata
import json
import os
import random
import re
import string
import subprocess
import collections
from pathlib import Path
from typing import List, Tuple, Optional
from urllib.parse import urlparse


# ─── Dataset 1: Linux Kernel File Paths ──────────────────────────────────────

def load_linux_kernel_paths(repo_path: Optional[str] = None, 
                             max_paths: int = 100000) -> List[str]:
    """
    Loads real hierarchical file paths from a local Linux kernel git repository.
    If no repo_path is provided, generates a realistic synthetic filesystem hierarchy 
    that mirrors the Linux kernel directory structure.
    
    Returns:
        Sorted list of path strings.
    """
    if repo_path and os.path.exists(repo_path):
        try:
            result = subprocess.run(
                ["git", "ls-tree", "-r", "--name-only", "HEAD"],
                cwd=repo_path,
                capture_output=True, text=True, timeout=60
            )
            if result.returncode == 0:
                paths = [f"/{line.strip()}" for line in result.stdout.strip().split('\n') if line.strip()]
                paths = sorted(paths)[:max_paths]
                if len(paths) > 100:
                    return paths
        except Exception:
            pass
    
    # Fallback: generate realistic Linux-kernel-style paths
    return _generate_linux_style_paths(max_paths)


def _generate_linux_style_paths(num_paths: int) -> List[str]:
    """Generates paths mimicking real Linux kernel directory structure."""
    rng = random.Random(2024)
    
    # Real top-level Linux kernel directories
    top_dirs = [
        "arch", "block", "certs", "crypto", "Documentation", "drivers",
        "firmware", "fs", "include", "init", "ipc", "kernel", "lib",
        "mm", "net", "samples", "scripts", "security", "sound",
        "tools", "usr", "virt"
    ]
    
    # Real second-level directories for depth
    arch_subdirs = ["arm", "arm64", "x86", "mips", "riscv", "powerpc", "s390", "sparc"]
    driver_subdirs = [
        "acpi", "ata", "base", "block", "bluetooth", "bus", "char", "clk",
        "clocksource", "cpufreq", "crypto", "dma", "edac", "firmware",
        "gpio", "gpu", "hid", "hwmon", "i2c", "iio", "infiniband",
        "input", "iommu", "irqchip", "leds", "md", "media", "memory",
        "mfd", "misc", "mmc", "mtd", "net", "nvdimm", "nvme", "of",
        "pci", "perf", "phy", "pinctrl", "platform", "power", "pps",
        "pwm", "regulator", "reset", "rtc", "scsi", "soc", "spi",
        "staging", "target", "thermal", "thunderbolt", "tty", "usb",
        "vfio", "vhost", "video", "virtio", "watchdog"
    ]
    fs_subdirs = [
        "btrfs", "ceph", "debugfs", "devpts", "ecryptfs", "ext2", "ext4",
        "f2fs", "fat", "fuse", "gfs2", "hfs", "hfsplus", "jbd2", "jffs2",
        "kernfs", "lockd", "nfs", "nfsd", "nilfs2", "ntfs", "ocfs2",
        "overlayfs", "proc", "quota", "sysfs", "tmpfs", "ubifs", "udf", "xfs"
    ]
    net_subdirs = [
        "batman-adv", "bluetooth", "bridge", "can", "ceph", "core",
        "dccp", "dns_resolver", "ethernet", "ipv4", "ipv6", "mac80211",
        "netfilter", "nfc", "openvswitch", "packet", "rfkill", "sched",
        "sctp", "sunrpc", "tipc", "unix", "wireless", "xfrm"
    ]
    
    file_extensions = [".c", ".h", ".S", ".dts", ".dtsi", ".rst", ".txt", ".py", ".sh", ".pl"]
    
    component_words = [
        "core", "init", "main", "utils", "helper", "common", "base", "ops",
        "mgmt", "ctrl", "data", "io", "mem", "buf", "alloc", "free",
        "lock", "sync", "irq", "dma", "pci", "usb", "spi", "i2c",
        "timer", "clock", "reset", "power", "debug", "trace", "log",
        "config", "setup", "probe", "remove", "suspend", "resume",
        "read", "write", "open", "close", "ioctl", "mmap", "poll",
        "queue", "ring", "desc", "cmd", "status", "error", "fault",
        "page", "slab", "cache", "node", "zone", "block", "sector"
    ]
    
    paths = set()
    
    while len(paths) < num_paths:
        top = rng.choice(top_dirs)
        depth = rng.randint(2, 7)
        
        parts = [top]
        
        # Add realistic second-level directory
        if top == "arch":
            parts.append(rng.choice(arch_subdirs))
        elif top == "drivers":
            parts.append(rng.choice(driver_subdirs))
        elif top == "fs":
            parts.append(rng.choice(fs_subdirs))
        elif top == "net":
            parts.append(rng.choice(net_subdirs))
        else:
            parts.append(rng.choice(component_words))
        
        # Add additional depth
        for _ in range(depth - 2):
            word = rng.choice(component_words)
            if rng.random() < 0.3:
                word += str(rng.randint(0, 9))
            parts.append(word)
        
        # Add filename
        filename = rng.choice(component_words)
        if rng.random() < 0.4:
            filename += "_" + rng.choice(component_words)
        filename += rng.choice(file_extensions)
        parts.append(filename)
        
        path = "/" + "/".join(parts)
        paths.add(path)
    
    return sorted(paths)[:num_paths]


def load_filesystem_paths(root_path: Optional[str] = None, max_paths: int = 100000) -> List[str]:
    """
    Loads real file and directory paths from a local filesystem tree.

    Paths are normalized into slash-prefixed hierarchical keys. The walk is
    bounded by max_paths and skips generated/cache directories so benchmarks do
    not accidentally spend most of their time on tool artifacts.
    """
    root = os.path.abspath(root_path or os.getcwd())
    skip_dirs = {
        ".git",
        ".hg",
        ".svn",
        ".pytest_cache",
        "__pycache__",
        "node_modules",
        ".venv",
        "venv",
        "dist",
        "build",
        "target",
    }
    paths: set[str] = set()

    for current_root, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in skip_dirs]
        rel_root = os.path.relpath(current_root, root)
        if rel_root != ".":
            paths.add("/fs/" + rel_root.replace(os.sep, "/"))
        for name in files:
            rel_path = os.path.normpath(os.path.join(rel_root, name))
            if rel_path.startswith(".."):
                continue
            paths.add("/fs/" + rel_path.replace(os.sep, "/").lstrip("./"))
            if len(paths) >= max_paths:
                return sorted(paths)[:max_paths]

    return sorted(paths)[:max_paths]


def generate_url_paths(num_urls: int = 100000, seed: int = 2024) -> List[str]:
    """
    Generates URL corpus keys with reversed host hierarchy and path segments.

    Example: /url/https/com/example/www/api/v1/users/42
    """
    rng = random.Random(seed)
    schemes = ["https", "http"]
    tlds = ["com", "org", "net", "io", "dev", "edu", "co", "ai"]
    domains = [
        "example", "shop", "docs", "cloud", "search", "media", "maps",
        "auth", "payments", "analytics", "cdn", "registry", "packages",
    ]
    subdomains = ["www", "api", "static", "assets", "app", "admin", "edge", "m"]
    segments = [
        "v1", "v2", "users", "teams", "projects", "repos", "issues",
        "pulls", "assets", "images", "docs", "guide", "reference", "search",
        "checkout", "orders", "invoices", "download", "release", "latest",
    ]

    paths: set[str] = set()
    while len(paths) < num_urls:
        scheme = rng.choice(schemes)
        tld = rng.choice(tlds)
        domain = rng.choice(domains)
        host = [tld, domain]
        if rng.random() < 0.75:
            host.append(rng.choice(subdomains))
        depth = rng.choices([1, 2, 3, 4, 5, 6], weights=[5, 15, 30, 25, 15, 10])[0]
        url_parts = [rng.choice(segments) for _ in range(depth)]
        if rng.random() < 0.45:
            url_parts.append(str(rng.randint(1, 250000)))
        if rng.random() < 0.25:
            url_parts.append(f"q_{rng.choice(segments)}")
        paths.add("/url/" + scheme + "/" + "/".join(host + url_parts))

    return sorted(paths)[:num_urls]


def generate_package_manager_paths(num_paths: int = 100000, seed: int = 2024) -> List[str]:
    """
    Generates package-manager artifact paths for npm, PyPI, Maven, and crates.

    These keys model dependency registries where package namespace, version,
    artifact type, and file path form a deep hierarchy.
    """
    rng = random.Random(seed)
    ecosystems = ["npm", "pypi", "maven", "crates"]
    package_words = [
        "core", "client", "server", "utils", "parser", "runtime", "sdk",
        "plugin", "driver", "adapter", "crypto", "storage", "index",
        "schema", "lint", "build", "test", "web", "native", "data",
    ]
    orgs = ["apache", "spring", "google", "microsoft", "meta", "openjs", "rustlang", "acme"]
    files = ["README.md", "LICENSE", "package.json", "pyproject.toml", "Cargo.toml", "pom.xml", "src/lib.rs", "dist/index.js"]

    paths: set[str] = set()
    while len(paths) < num_paths:
        eco = rng.choice(ecosystems)
        name = rng.choice(package_words) + "-" + rng.choice(package_words)
        version = f"{rng.randint(0, 8)}.{rng.randint(0, 30)}.{rng.randint(0, 99)}"
        if eco == "npm":
            scope = rng.choice(["@" + rng.choice(orgs), "unscoped"])
            artifact = rng.choice(["tarball", "metadata", "dist", "types"])
            path = f"/pkg/npm/{scope}/{name}/{version}/{artifact}/{rng.choice(files)}"
        elif eco == "pypi":
            pyver = rng.choice(["py3", "cp310", "cp311", "cp312"])
            artifact = rng.choice(["sdist", "wheel", "metadata"])
            path = f"/pkg/pypi/{name}/{version}/{pyver}/{artifact}/{rng.choice(files)}"
        elif eco == "maven":
            group = rng.choice(orgs)
            artifact = rng.choice(package_words) + "-" + rng.choice(["api", "core", "bom", "parent"])
            ext = rng.choice(["jar", "pom", "sources.jar", "javadoc.jar"])
            path = f"/pkg/maven/{group}/{name}/{artifact}/{version}/{ext}"
        else:
            target = rng.choice(["src", "docs", "examples", "benches"])
            path = f"/pkg/crates/{name}/{version}/{target}/{rng.choice(files)}"
        paths.add(path)

    return sorted(paths)[:num_paths]


# ─── Dataset 2: DNS Zone Records ─────────────────────────────────────────────

def generate_dns_paths(num_records: int = 100000, seed: int = 2024) -> List[str]:
    """
    Generates realistic DNS-style hierarchical records.
    DNS has an inherently tree-like structure (TLD → domain → subdomain → host).
    
    Returns:
        Sorted list of reverse-DNS path strings (e.g., /com/google/mail/smtp).
    """
    rng = random.Random(seed)
    
    tlds = ["com", "org", "net", "edu", "gov", "io", "dev", "ai", "co", "us", "uk", "de", "jp", "cn"]
    
    domains_by_tld = {
        "com": ["google", "amazon", "microsoft", "apple", "meta", "netflix", "uber", 
                "spotify", "slack", "zoom", "adobe", "oracle", "ibm", "intel", "nvidia",
                "cloudflare", "fastly", "akamai", "stripe", "shopify", "twilio"],
        "org": ["wikipedia", "mozilla", "apache", "linux", "debian", "ubuntu", "gnome",
                "kde", "fsf", "eff", "archive", "ietf", "w3c", "openssl"],
        "net": ["cloudfront", "akamai", "fastly", "linode", "vultr", "hetzner", "ovh"],
        "edu": ["mit", "stanford", "cmu", "berkeley", "caltech", "harvard", "yale", "princeton"],
        "io": ["github", "gitlab", "bitbucket", "heroku", "fly", "render", "railway"],
        "dev": ["web", "android", "flutter", "chrome", "firebase"],
    }
    
    # Fill missing TLDs with generic domains
    for tld in tlds:
        if tld not in domains_by_tld:
            domains_by_tld[tld] = [f"domain{i}" for i in range(10)]
    
    service_words = [
        "www", "api", "mail", "smtp", "imap", "pop3", "ftp", "ssh", "dns",
        "ns1", "ns2", "ns3", "cdn", "static", "media", "img", "video",
        "auth", "login", "sso", "oauth", "admin", "dashboard", "console",
        "db", "redis", "cache", "queue", "worker", "cron", "scheduler",
        "gateway", "proxy", "lb", "ingress", "edge", "node", "cluster",
        "staging", "prod", "dev", "test", "qa", "uat", "beta", "canary",
        "us-east-1", "us-west-2", "eu-west-1", "ap-south-1", "ap-northeast-1",
        "monitoring", "metrics", "logs", "traces", "alerts", "grafana",
        "ci", "cd", "build", "deploy", "release", "artifact", "registry"
    ]
    
    paths = set()
    
    while len(paths) < num_records:
        tld = rng.choice(tlds)
        domain = rng.choice(domains_by_tld[tld])
        
        depth = rng.choices([1, 2, 3, 4, 5], weights=[10, 30, 35, 20, 5])[0]
        
        parts = [tld, domain]
        
        for _ in range(depth):
            svc = rng.choice(service_words)
            if rng.random() < 0.15:
                svc += str(rng.randint(1, 99))
            parts.append(svc)
        
        path = "/" + "/".join(parts)
        paths.add(path)
    
    return sorted(paths)[:num_records]


# ─── Dataset 3: JSON Document Paths (MongoDB-style) ──────────────────────────

def generate_json_paths(num_paths: int = 100000, seed: int = 2024) -> List[str]:
    """
    Generates realistic JSON document paths mimicking MongoDB nested document schemas.
    Example: /users/profile/settings/notifications/email
    
    Returns:
        Sorted list of JSON path strings.
    """
    rng = random.Random(seed)
    
    # Top-level collections
    collections = [
        "users", "products", "orders", "sessions", "analytics", "logs",
        "configs", "permissions", "workflows", "pipelines", "experiments",
        "models", "datasets", "features", "embeddings", "indexes",
        "notifications", "payments", "subscriptions", "invoices"
    ]
    
    # Nested field names
    fields = {
        "users": ["profile", "settings", "preferences", "activity", "security",
                  "billing", "notifications", "connections", "history", "metadata"],
        "products": ["details", "pricing", "inventory", "reviews", "images",
                     "variants", "categories", "shipping", "analytics", "seo"],
        "orders": ["items", "shipping", "payment", "tracking", "returns",
                   "discounts", "taxes", "fulfillment", "notes", "history"],
    }
    
    generic_fields = [
        "id", "name", "type", "status", "value", "count", "timestamp",
        "created_at", "updated_at", "deleted_at", "version", "schema",
        "parent", "children", "tags", "labels", "annotations", "metadata",
        "config", "options", "params", "args", "result", "output",
        "source", "target", "origin", "destination", "path", "url",
        "key", "secret", "token", "hash", "signature", "checksum",
        "min", "max", "avg", "sum", "total", "limit", "offset", "page",
        "width", "height", "size", "weight", "length", "depth", "level"
    ]
    
    paths = set()
    
    while len(paths) < num_paths:
        collection = rng.choice(collections)
        depth = rng.choices([2, 3, 4, 5, 6, 7], weights=[5, 20, 35, 25, 10, 5])[0]
        
        parts = [collection]
        
        # First nested field uses collection-specific vocabulary if available
        if collection in fields:
            parts.append(rng.choice(fields[collection]))
        else:
            parts.append(rng.choice(generic_fields))
        
        for _ in range(depth - 1):
            field = rng.choice(generic_fields)
            if rng.random() < 0.2:
                # Array-like indexing
                field += f"[{rng.randint(0, 99)}]"
            parts.append(field)
        
        path = "/" + "/".join(parts)
        paths.add(path)
    
    return sorted(paths)[:num_paths]


# ─── Dataset 4: Scalable Synthetic Tree ──────────────────────────────────────

def generate_scalable_tree(num_nodes: int = 100000, 
                            max_depth: int = 10,
                            min_branching: int = 2,
                            max_branching: int = 32,
                            seed: int = 2024) -> List[str]:
    """
    Generates a large-scale synthetic hierarchical tree dataset.
    Uses compact base-36 naming for efficiency at scale.
    """
    rng = random.Random(seed)
    
    paths = ["/root"]
    queue = collections.deque([("/root", 0)])
    
    while queue and len(paths) < num_nodes:
        parent_path, depth = queue.popleft()
        
        if depth >= max_depth:
            continue
        
        # Increase branching factor for larger scale
        num_children = rng.randint(min_branching, max_branching)
        
        for i in range(num_children):
            if len(paths) >= num_nodes:
                break
            # Use compact naming: base-36 encoded node ID
            child_name = _base36(len(paths))
            child_path = f"{parent_path}/{child_name}"
            paths.append(child_path)
            queue.append((child_path, depth + 1))
            
            # Periodically shuffle queue to keep tree somewhat balanced but random
            if len(paths) % 1000 == 0:
                random.shuffle(queue)
    
    return sorted(paths)


# ─── Dataset 5: Wikipedia Category Paths ─────────────────────────────────────

def load_wikipedia_categories(num_paths: int = 100000, seed: int = 2024) -> List[str]:
    """
    Generates realistic Wikipedia-style category paths.
    Example: /Main_topic_classifications/Physical_sciences/Physics/Classical_mechanics
    """
    rng = random.Random(seed)
    
    categories = {
        "Main_topic_classifications": ["Culture", "Geography", "Health", "History", "Mathematics", "Nature", "People", "Philosophy", "Religion", "Society", "Technology"],
        "Culture": ["Arts", "Language", "Literature", "Music", "Philosophy", "Sports"],
        "Geography": ["Africa", "Americas", "Antarctica", "Asia", "Europe", "Oceania"],
        "History": ["Ancient_history", "Modern_history", "By_region", "By_topic"],
        "Mathematics": ["Algebra", "Analysis", "Geometry", "Logic", "Number_theory", "Statistics"],
        "Nature": ["Animals", "Plants", "Space", "Earth_sciences", "Physical_sciences"],
        "Physical_sciences": ["Astronomy", "Chemistry", "Physics", "Earth_sciences"],
        "Physics": ["Classical_mechanics", "Electromagnetism", "Relativity", "Quantum_mechanics", "Thermodynamics"],
        "Technology": ["Computing", "Electronics", "Engineering", "Transport", "Biotechnology"]
    }
    
    all_cat_names = list(categories.keys()) + [item for sublist in categories.values() for item in sublist]
    all_cat_names = list(set(all_cat_names))
    
    paths = set()
    while len(paths) < num_paths:
        # Start with a random top-level-ish category
        curr = "Main_topic_classifications"
        path = ["", curr]
        
        depth = rng.randint(2, 6)
        for _ in range(depth):
            if curr in categories:
                next_cat = rng.choice(categories[curr])
            else:
                # Leaf or generic branch
                next_cat = rng.choice(all_cat_names)
            
            path.append(next_cat)
            curr = next_cat
        
        paths.add("/".join(path))
        
    return sorted(paths)[:num_paths]


# ─── Dataset 6: SOSD-style Integer Key Wrapper ───────────────────────────────

class IntegerKeyWrapper:
    """
    Wraps a list of integers to mimic hierarchical strings for fair comparison.
    Converts integers to fixed-width strings.
    """
    def __init__(self, keys: List[int]):
        self.raw_keys = sorted(keys)
        self.string_keys = [f"/{k:012d}" for k in self.raw_keys]
    
    def get_keys(self) -> List[str]:
        return self.string_keys


def _dedupe_sorted_limit(paths: List[str], max_paths: int) -> List[str]:
    return sorted(set(p for p in paths if p))[:max_paths]


_URL_RE = re.compile(r"https?://[^\s<>()\[\]{}\"'`]+")


def load_url_paths(source_root: Optional[str] = None, max_paths: int = 100000) -> List[str]:
    """
    Extract URL hierarchies from local text-like files.

    This produces keys such as /url/org/arxiv/abs/2407.11556, preserving domain
    hierarchy and URL path hierarchy without reading private browser history.
    """
    extensions = {".md", ".txt", ".rst", ".html", ".htm", ".json", ".bib", ".tex"}
    paths: List[str] = []
    
    # List of directories to search. First search project source_root, then system docs if needed.
    search_dirs = []
    if source_root:
        search_dirs.append(Path(source_root).resolve())
    else:
        search_dirs.append(Path(os.getcwd()).resolve())
        
    system_doc_dir = Path("/usr/share/doc")
    if system_doc_dir.exists():
        search_dirs.append(system_doc_dir)

    file_count = 0
    max_system_files = 1000  # Cap system file scans to keep it fast
    
    for search_dir in search_dirs:
        if len(paths) >= max_paths:
            break
        for root, _, files in os.walk(search_dir):
            if len(paths) >= max_paths:
                break
            if any(part in {".git", "__pycache__", ".pytest_cache", "node_modules"} for part in root.split(os.sep)):
                continue
                
            for file in sorted(files):
                file_path = Path(root) / file
                suffix = file_path.suffix.lower()
                is_gz = suffix == ".gz"
                real_suffix = Path(file_path.stem).suffix.lower() if is_gz else suffix
                
                # We also process copyright files and files without extension (like LICENSE)
                if not (real_suffix in extensions or file.lower() in {"copyright", "license", "readme"}):
                    continue
                    
                file_count += 1
                if search_dir == system_doc_dir and file_count > max_system_files:
                    break
                    
                try:
                    if is_gz:
                        import gzip
                        with gzip.open(file_path, "rt", encoding="utf-8", errors="ignore") as f:
                            text = f.read()
                    else:
                        text = file_path.read_text(encoding="utf-8", errors="ignore")
                except OSError:
                    continue
                    
                for raw_url in _URL_RE.findall(text):
                    parsed = urlparse(raw_url.rstrip(".,;:"))
                    if not parsed.netloc:
                        continue
                    domain_parts = list(reversed([p for p in parsed.netloc.lower().split(".") if p]))
                    path_parts = [p for p in parsed.path.strip("/").split("/") if p]
                    key_parts = ["url", *domain_parts, *path_parts]
                    if parsed.query:
                        query_keys = sorted(part.split("=", 1)[0] for part in parsed.query.split("&") if part)
                        key_parts.extend(["query", *query_keys[:8]])
                    paths.append("/" + "/".join(key_parts))
                    if len(paths) >= max_paths:
                        break
                        
    if paths:
        return _dedupe_sorted_limit(paths, max_paths)
    return _generate_url_style_paths(max_paths)


def load_dns_paths_from_urls(source_root: Optional[str] = None, max_paths: int = 100000) -> List[str]:
    """Build a DNS-style corpus from domains found in local URL references."""
    url_paths = load_url_paths(source_root, max_paths=max_paths * 2)
    dns_paths: List[str] = []
    for key in url_paths:
        parts = [p for p in key.split("/") if p]
        if len(parts) < 3 or parts[0] != "url":
            continue
        domain_parts = []
        for part in parts[1:]:
            if part in {"abs", "pdf", "query"}:
                break
            domain_parts.append(part)
            if len(domain_parts) >= 4:
                break
        if domain_parts:
            dns_paths.append("/dns/" + "/".join(domain_parts))
    return _dedupe_sorted_limit(dns_paths, max_paths) or generate_dns_paths(max_paths)


def load_json_document_paths(source_root: Optional[str] = None, max_paths: int = 100000) -> List[str]:
    """Extract real JSON pointer-like paths from local JSON result/data files."""
    paths: List[str] = []
    search_dirs = []
    if source_root:
        search_dirs.append(Path(source_root).resolve())
    else:
        search_dirs.append(Path(os.getcwd()).resolve())
        
    system_share_dir = Path("/usr/share")
    if system_share_dir.exists():
        search_dirs.append(system_share_dir)

    json_file_count = 0
    max_json_files = 300  # Cap JSON scans to keep it fast
    
    for search_dir in search_dirs:
        if len(paths) >= max_paths:
            break
        for root, _, files in os.walk(search_dir):
            if len(paths) >= max_paths:
                break
            if any(part in {".git", "__pycache__", ".pytest_cache", "node_modules"} for part in root.split(os.sep)):
                continue
            for file in files:
                if not file.lower().endswith(".json"):
                    continue
                file_path = Path(root) / file
                json_file_count += 1
                if search_dir == system_share_dir and json_file_count > max_json_files:
                    break
                try:
                    with file_path.open("r", encoding="utf-8") as handle:
                        payload = json.load(handle)
                except (OSError, json.JSONDecodeError):
                    continue
                stem = file_path.stem.replace(" ", "_")
                _collect_json_paths(payload, f"/json/{stem}", paths, max_paths)
                if len(paths) >= max_paths:
                    break
                    
    return _dedupe_sorted_limit(paths, max_paths) or generate_json_paths(max_paths)


def _collect_json_paths(value, prefix: str, out: List[str], max_paths: int) -> None:
    if len(out) >= max_paths:
        return
    out.append(prefix)
    if isinstance(value, dict):
        for key in sorted(value):
            _collect_json_paths(value[key], f"{prefix}/{str(key)}", out, max_paths)
            if len(out) >= max_paths:
                return
    elif isinstance(value, list):
        for item in value[:32]:
            _collect_json_paths(item, f"{prefix}/[]", out, max_paths)
            if len(out) >= max_paths:
                return


def load_package_manager_paths(max_paths: int = 100000) -> List[str]:
    """
    Load package-manager path keys from installed Python distribution metadata or system libraries.

    Each key is shaped like /pkg/pypi/<distribution>/<file>.
    """
    paths: List[str] = []
    
    # 1. Try python dist metadata
    try:
        for dist in sorted(importlib_metadata.distributions(), key=lambda d: (d.metadata.get("Name") or "").lower()):
            name = (dist.metadata.get("Name") or "unknown").lower().replace("_", "-")
            if not name:
                continue
            paths.append(f"/pkg/pypi/{name}")
            files = list(dist.files or [])[:64]
            for file in files:
                paths.append(f"/pkg/pypi/{name}/{str(file).replace(os.sep, '/')}")
                if len(paths) >= max_paths:
                    return _dedupe_sorted_limit(paths, max_paths)
    except Exception:
        pass
        
    # 2. Try walking /usr/lib/python3/dist-packages/ and /usr/lib/python3.13/ if paths are insufficient
    system_libs = [Path("/usr/lib/python3/dist-packages"), Path("/usr/lib/python3")]
    for lib_dir in system_libs:
        if len(paths) >= max_paths:
            break
        if not lib_dir.exists():
            continue
        # Walk directories up to 3 levels deep
        for root, dirs, files in os.walk(lib_dir):
            if len(paths) >= max_paths:
                break
            rel_root = Path(root).relative_to(lib_dir)
            parts = rel_root.parts
            if not parts:
                continue
            # Treat the first level directory/package as the package/distribution name
            pkg_name = parts[0].replace("_", "-").lower()
            if pkg_name in {"__pycache__", "dist-packages", "site-packages"}:
                continue
            pkg_rel_path = "/".join(parts[1:])
            for file in files:
                if file.endswith(".pyc") or file == "__pycache__":
                    continue
                file_rel = f"{pkg_rel_path}/{file}".strip("/")
                paths.append(f"/pkg/pypi/{pkg_name}/{file_rel}")
                if len(paths) >= max_paths:
                    break

    return _dedupe_sorted_limit(paths, max_paths) or _generate_package_style_paths(max_paths)



def _generate_url_style_paths(num_paths: int, seed: int = 2024) -> List[str]:
    rng = random.Random(seed)
    domains = ["arxiv.org", "github.com", "docs.python.org", "pypi.org", "vldb.org", "sigmod.org"]
    sections = ["abs", "pdf", "repos", "docs", "project", "paper", "api", "release"]
    paths = set()
    while len(paths) < num_paths:
        domain = list(reversed(rng.choice(domains).split(".")))
        depth = rng.randint(2, 6)
        parts = ["url", *domain]
        for _ in range(depth):
            parts.append(rng.choice(sections))
            if rng.random() < 0.4:
                parts.append(str(rng.randint(1, 9999)))
        paths.add("/" + "/".join(parts))
    return sorted(paths)


def _generate_package_style_paths(num_paths: int, seed: int = 2024) -> List[str]:
    rng = random.Random(seed)
    ecosystems = ["pypi", "npm", "crates", "maven"]
    packages = ["numpy", "pytest", "react", "vite", "serde", "tokio", "junit", "postgres"]
    files = ["metadata", "dist-info", "src", "lib", "tests", "README.md", "package.json", "pyproject.toml"]
    paths = set()
    while len(paths) < num_paths:
        parts = ["pkg", rng.choice(ecosystems), rng.choice(packages), rng.choice(files)]
        if rng.random() < 0.6:
            parts.append(f"file_{rng.randint(1, 9999):04d}")
        paths.add("/" + "/".join(parts))
    return sorted(paths)


def _base36(n: int) -> str:
    """Encodes an integer as a compact base-36 string (0-9, a-z)."""
    if n == 0:
        return "n0"
    chars = string.digits + string.ascii_lowercase
    result = []
    while n > 0:
        result.append(chars[n % 36])
        n //= 36
    return "n" + "".join(reversed(result))


# ─── Workload Generators ─────────────────────────────────────────────────────

def generate_ycsb_workload(sorted_paths: List[str], 
                            num_ops: int = 10000,
                            read_ratio: float = 0.5,
                            insert_ratio: float = 0.5,
                            locality: str = "uniform",
                            seed: int = 42) -> List[dict]:
    """
    Generates YCSB-style mixed read/write workloads.
    
    Args:
        sorted_paths: Current sorted key list
        num_ops: Total number of operations
        read_ratio: Fraction of reads (rest are inserts)
        insert_ratio: Fraction of inserts
        locality: "uniform", "zipfian", or "hotspot"
        seed: Random seed
        
    Returns:
        List of operation dicts: {"type": "read"|"insert"|"delete", "key": str, ...}
    """
    rng = random.Random(seed)
    N = len(sorted_paths)
    ops = []
    insert_counter = 0
    
    for _ in range(num_ops):
        roll = rng.random()
        
        if roll < read_ratio:
            # Read operation
            if locality == "zipfian":
                # Zipf-like: favor low-index keys (hot keys)
                idx = int(N * (rng.random() ** 2))
            elif locality == "hotspot":
                # 80% of reads hit 20% of keys
                if rng.random() < 0.8:
                    idx = rng.randint(0, max(0, N // 5 - 1))
                else:
                    idx = rng.randint(0, N - 1)
            else:
                idx = rng.randint(0, N - 1)
            
            ops.append({"type": "read", "key": sorted_paths[min(idx, N - 1)]})
            
        else:
            # Insert operation: create a new key under an existing parent
            parent_idx = rng.randint(0, N - 1)
            parent_path = sorted_paths[parent_idx]
            new_key = f"{parent_path}/ins_{insert_counter:06d}"
            insert_counter += 1
            ops.append({"type": "insert", "key": new_key, "parent_key": parent_path})
    
    return ops


# ─── Utility ──────────────────────────────────────────────────────────────────

def dataset_stats(paths: List[str]) -> dict:
    """Computes statistics about a hierarchical path dataset."""
    depths = [p.count('/') for p in paths]
    
    # Compute branching factor distribution
    parent_children = collections.Counter()
    for p in paths:
        parent = '/'.join(p.split('/')[:-1])
        if parent:
            parent_children[parent] += 1
    
    branching = list(parent_children.values()) if parent_children else [0]
    
    return {
        "num_keys": len(paths),
        "min_depth": min(depths),
        "max_depth": max(depths),
        "avg_depth": sum(depths) / len(depths),
        "num_unique_parents": len(parent_children),
        "avg_branching": sum(branching) / max(1, len(branching)),
        "max_branching": max(branching) if branching else 0,
        "avg_key_length": sum(len(p) for p in paths) / len(paths),
    }


if __name__ == "__main__":
    print("=" * 70)
    print(" CD-HLI Dataset Loader Test Suite")
    print("=" * 70)
    
    datasets = {
        "Local Filesystem": load_filesystem_paths(max_paths=50000),
        "Linux Kernel (Synthetic)": load_linux_kernel_paths(max_paths=50000),
        "URL Paths": generate_url_paths(50000),
        "DNS Zone Records": generate_dns_paths(50000),
        "JSON Document Paths": generate_json_paths(50000),
        "Package Manager Paths": generate_package_manager_paths(50000),
        "Scalable Synthetic Tree (100K)": generate_scalable_tree(100000),
    }
    
    for name, paths in datasets.items():
        stats = dataset_stats(paths)
        print(f"\n📊 {name}:")
        print(f"   Keys: {stats['num_keys']:,}")
        print(f"   Depth range: [{stats['min_depth']}, {stats['max_depth']}] (avg {stats['avg_depth']:.1f})")
        print(f"   Avg branching: {stats['avg_branching']:.1f} (max {stats['max_branching']})")
        print(f"   Avg key length: {stats['avg_key_length']:.0f} chars")
        print(f"   Sample paths:")
        for p in paths[:3]:
            print(f"     {p}")
        print(f"     ...")
        for p in paths[-2:]:
            print(f"     {p}")
