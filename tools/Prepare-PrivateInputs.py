"""Prepare ignored LOCAL fixtures for offline tests/builds; no network or hardware.

Input is the original research/tu102 directory or a private backup of it.
This never extracts firmware from a device and never installs an EFI image.
"""
import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO / 'research/tu102'
INPUTS = {
    'captures/capture-04/LIVE-PROM.BIN': (524288, 'd88ee7d835cb5be26b9c606ae2a6ae51337537fc46c146d777eed0291d431a43'),
    'captures/capture-04/NATIVE-FWSEC.BIN': (39440, '1516eb686e3b4a379f9c5739720b4ebf938700c2b5bf65686006ccd3999e2227'),
    'assets/upstream-cmp50hx-efi/bl_gsp_tu102.bin': (768, 'f21f1cfbb8fffa8067685314367fc4c50f3f84dd99eda7c19db4292d9269b9d9'),
    'assets/upstream-cmp50hx-efi/booter_ucode_prod.bin': (59136, 'e0f0fc93da097b5b68e5af3adb58b2235ed1fad5fce0b23592d3895af44cb743'),
    'assets/upstream-cmp50hx-efi/v67_payload.bin': (64000, 'de7288bcfb80c2d4b259a59e823c926ba0a9adcab594219cc6802c94257ce271'),
    'assets/upstream-cmp50hx-efi/gsp_rm_boot_dbg.bin': (131072, 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471'),
    'build/transaction/capture-19/ROOT-MAP-FIRST96.bin': (96, 'ca5165fd369ab13cbd5dc9d2d1f9b4cc989b09ec81072ed5093bdc691bffd132'),
    'build/transaction/amd-platform/WINDOWS-IVRS.bin': (200, 'a977843575fcb5a18b44d25c2d068c4e5d6c55f8683ac2b7b91642bc604b6652'),
    'build/transaction/automatic-platform/WINDOWS-BOOT0000.bin': (300, '9ded5321fc59e15cf7d024fe088fb19d78ac7e31d8eaef7392e0ab5425213d33'),
}


def prepare(source):
    source = Path(source).resolve()
    # Validate every file before creating output. Input hashes document one
    # tested system, not an arbitrary-card enrollment or override mechanism.
    blobs = {}
    for name, (size, expected) in INPUTS.items():
        path = source / name
        if not path.is_file():
            raise ValueError('Private input missing: '+name)
        data = path.read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Private input differs from measured local profile: '+name)
        blobs[name] = data
    header = (source / 'transaction/internal_auto.h').read_text(encoding='utf-8')
    match = re.search(r'static const UINT8 signature\[16\]=\{([^}]+)\};', header)
    if not match:
        raise ValueError('Original private ESP binding is missing')
    values = [int(s.strip(), 0) for s in match[1].split(',')]
    if len(values) != 16 or any(not 0 <= n <= 255 for n in values):
        raise ValueError('Invalid private ESP signature')
    for name, data in blobs.items():
        path = ROOT / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    build = ROOT / 'build'
    (build / 'local_bindings.h').write_text(
        '/* PRIVATE generated local binding. Do not commit. */\n'
        '#define TU102_LOCAL_ESP_SIGNATURE {' + ','.join(hex(n) for n in values) + '}\n', encoding='utf-8')
    symbols = [
        ('asset_native', 'captures/capture-04/NATIVE-FWSEC.BIN'),
        ('asset_generic', 'assets/upstream-cmp50hx-efi/bl_gsp_tu102.bin'),
        ('asset_booter', 'assets/upstream-cmp50hx-efi/booter_ucode_prod.bin'),
        ('asset_signature', 'assets/upstream-cmp50hx-efi/v67_payload.bin'),
        ('asset_boot', 'assets/upstream-cmp50hx-efi/gsp_rm_boot_dbg.bin'),
    ]
    lines = ['/* PRIVATE generated payloads. Do not commit/distribute. */']
    for symbol, name in symbols:
        data = blobs[name]
        lines.append(f'static const uint8_t {symbol}[{len(data)}]={{')
        lines.extend(','.join(f'0x{b:02x}' for b in data[i:i+32])+',' for i in range(0,len(data),32))
        lines.append('};')
    (build / 'transaction_assets.h').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    (build / 'private-input-hashes.json').write_text(json.dumps({k:v[1] for k,v in INPUTS.items()},indent=2)+'\n', encoding='utf-8')
    print('Private inputs checked and staged into ignored paths. No hardware access.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    prepare(parser.parse_args().source)
