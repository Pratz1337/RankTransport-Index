"""Extract a small prefix of the verified natural split for driver regression."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('source', type=Path)
p.add_argument('destination', type=Path)
a = p.parse_args()
a.destination.mkdir(parents=True, exist_ok=True)
rows = []
for suffix, count in [('initial', 199900), ('insert', 100)]:
    src = a.source / f'commoncrawl_hosts_200m_{suffix}.txt'
    dst = a.destination / f'{suffix}.txt'
    if dst.exists():
        raise SystemExit(f'refusing to overwrite {dst}')
    h = hashlib.sha256()
    with src.open('rb') as stream, dst.open('wb') as out:
        written = 0
        for line in itertools.islice(stream, count):
            out.write(line)
            h.update(line)
            written += 1
    if written != count:
        raise SystemExit('source too short')
    rows.append({'source': str(src), 'path': str(dst), 'keys': count, 'sha256': h.hexdigest()})
(a.destination / 'provenance.json').write_text(json.dumps({
    'scope': '200000 natural keys, driver regression only; not flagship scale evidence',
    'files': rows}, indent=2) + '\n', encoding='utf-8')
print(json.dumps(rows))
