#!/usr/bin/env python3
"""Restore the exact public upstream assets recorded in UPSTREAM.json."""
import hashlib
import json
import urllib.request
from pathlib import Path
root=Path(__file__).resolve().parent
manifest=json.loads((root/'UPSTREAM.json').read_text())
for entry in manifest['files']:
    target=root/entry['path']
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest()==entry['sha256']:
        continue
    url='https://raw.githubusercontent.com/espressif/esp-sr/'+manifest['commit']+'/'+entry['source']
    with urllib.request.urlopen(url,timeout=30) as response:
        data=response.read()
    if hashlib.sha256(data).hexdigest()!=entry['sha256']:
        raise RuntimeError('Upstream hash mismatch: '+entry['path'])
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(data)
    print('Restored',entry['path'])
