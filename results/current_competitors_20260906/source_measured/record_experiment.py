"""Run one experiment and retain exact stdout, stderr, command and source hashes."""
import argparse
import hashlib
import json
import platform
import subprocess
import time
from pathlib import Path
try:
    import resource
except ImportError:
    resource = None


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--source', type=Path, action='append', default=[])
    p.add_argument('command', nargs=argparse.REMAINDER)
    a = p.parse_args()
    command = a.command[1:] if a.command[:1] == ['--'] else a.command
    if not command:
        p.error('experiment command is required')
    if a.output.exists():
        p.error('refusing to overwrite an existing experiment record')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    sources = {str(path): digest(path) for path in a.source}
    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN) if resource else None
    started = time.time()
    completed = subprocess.run(command, capture_output=True, text=True)
    record = {'command': command, 'platform': platform.platform(),
              'started_unix': started, 'elapsed_seconds': time.time() - started,
              'sources_sha256': sources, 'exit_code': completed.returncode,
              'stdout': completed.stdout, 'stderr': completed.stderr}
    if resource:
        usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        record['resource_usage'] = {
            'max_rss_kib': usage_after.ru_maxrss,
            'user_seconds': usage_after.ru_utime - usage_before.ru_utime,
            'system_seconds': usage_after.ru_stime - usage_before.ru_stime,
            'major_faults': usage_after.ru_majflt - usage_before.ru_majflt,
            'minor_faults': usage_after.ru_minflt - usage_before.ru_minflt,
            'voluntary_context_switches': usage_after.ru_nvcsw - usage_before.ru_nvcsw,
            'involuntary_context_switches': usage_after.ru_nivcsw - usage_before.ru_nivcsw,
        }
    try:
        record['result'] = json.loads(completed.stdout)
    except json.JSONDecodeError:
        record['result'] = None
    a.output.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'output': str(a.output), 'exit_code': completed.returncode,
                      'elapsed_seconds': record['elapsed_seconds'], 'result': record['result']}), flush=True)
    raise SystemExit(completed.returncode)


if __name__ == '__main__':
    main()
