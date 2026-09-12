#!/usr/bin/env python3
"""Verify pinned NuGet inputs and prepare an offline configuration (no downloads)."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('archives', type=Path)
args = parser.parse_args()
manifest = json.loads(Path(__file__).with_name('nuget-inputs.json').read_text())
for item in manifest['packages']:
    path = args.archives / item['archive']
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if path.stat().st_size != item['size'] or digest != item['sha256']:
        raise SystemExit(f"Incorrect NuGet input: {path}")
(args.archives / 'NuGet.Config').write_text('''<configuration>
  <packageSources><clear/><add key="fixed-offline-inputs" value="."/></packageSources>
</configuration>\n''')
print(f"Verified {len(manifest['packages'])} NuGet inputs")
