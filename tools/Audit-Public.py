"""Audit the PUBLIC directory itself; ignores neither build nor hidden files.

Run BEFORE building with private fixtures, or use a clean export for publication.
"""
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
FORBIDDEN_SUFFIXES={'.efi','.exe','.dll','.bin','.rom','.obj','.o','.map','.zip','.7z','.pyc','.pdb'}
FORBIDDEN_NAMES={'AUTO.ENABLED','TRANSACTION.TXT','AUTOMATIC-BOOT-STATE.json','local_bindings.h','transaction_assets.h'}
errors=[];count=0
for path in ROOT.rglob('*'):
    if not path.is_file() or '.git' in path.relative_to(ROOT).parts:
        continue
    rel=path.relative_to(ROOT)
    if path.suffix.lower() in FORBIDDEN_SUFFIXES or path.name in FORBIDDEN_NAMES or any(part in ('captures','assets','private','build','__pycache__') for part in rel.parts):
        errors.append(str(rel)+': generated/private artifact')
        continue
    raw=path.read_bytes();count+=1
    if raw.startswith(b'MZ') or b'\0' in raw:
        errors.append(str(rel)+': binary content')
        continue
    try:
        text=raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        errors.append(str(rel)+': non-UTF8 content');continue
    if re.search(r'(?i)[A-Z]:[\\/]Users[\\/][^\s/\\]+',text):
        errors.append(str(rel)+': personal Windows path')
    if re.search(r'(?i)(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----)',text):
        errors.append(str(rel)+': possible secret')
print(json.dumps(dict(passed=not errors, files_checked=count, errors=errors),indent=2))
if errors:
    raise SystemExit(1)
