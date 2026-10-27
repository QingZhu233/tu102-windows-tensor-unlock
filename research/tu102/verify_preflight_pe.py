"""Finalize an unsigned MinGW EFI and verify the exact resulting PE image.

MinGW ld emits an all-zero import terminator. The only post-link change is
clearing that empty import directory, after checking its contents are zero.
No GPU firmware, VBIOS, driver, or operating-system binary is processed.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def verify(source, destination, live_prom=False):
    data = bytearray(Path(source).read_bytes())

    def require(ok, message):
        if not ok:
            raise ValueError(message)

    def u16(at):
        require(0 <= at <= len(data) - 2, "truncated PE uint16")
        return struct.unpack_from("<H", data, at)[0]

    def u32(at):
        require(0 <= at <= len(data) - 4, "truncated PE uint32")
        return struct.unpack_from("<I", data, at)[0]

    require(data[:2] == b"MZ", "missing MZ")
    pe = u32(0x3C)
    require(data[pe:pe+4] == b"PE\0\0", "missing PE signature")
    require(u16(pe+4) == 0x8664, "not x86-64")
    opt = pe + 24
    require(u16(opt) == 0x20B and u16(pe+20) >= 240, "not PE32+")
    require(u16(opt+68) == 10, "not EFI application")
    require(struct.unpack_from("<Q", data, opt+24)[0] == 0, "image base must be zero")
    require(u16(opt+70) & 0x140 == 0x140, "NX/ASLR missing")
    require(u32(opt+108) >= 16, "data directories missing")
    require(not u16(pe+22) & 1, "relocations stripped")
    table = opt + u16(pe+20)
    sections = []
    for i in range(u16(pe+6)):
        off = table + 40*i
        require(off+40 <= len(data), "truncated section table")
        sections.append(dict(name=bytes(data[off:off+8]).rstrip(b"\0").decode("ascii"),
                             virtual=u32(off+12), virtual_size=u32(off+8),
                             raw=u32(off+20), raw_size=u32(off+16), flags=u32(off+36)))

    def rva_bytes(rva, size):
        for s in sections:
            delta = rva - s["virtual"]
            if 0 <= delta <= s["raw_size"] and size <= s["raw_size"]-delta:
                start = s["raw"] + delta
                require(start+size <= len(data), "section extends beyond file")
                return data[start:start+size]
        raise ValueError("directory RVA not backed by section data")

    entry = u32(opt+16)
    require(any(s["virtual"] <= entry < s["virtual"]+s["virtual_size"] and
                s["flags"] & 0x20000000 for s in sections), "entry not executable")
    require(not any(s["flags"] & 0x20000000 and s["flags"] & 0x80000000
                    for s in sections), "writable executable section")

    directory = opt+112
    imports, import_size = struct.unpack_from("<II", data, directory+8)
    if imports or import_size:
        require(imports and 20 <= import_size <= 512, "unexpected imports")
        require(not any(rva_bytes(imports, import_size)), "nonempty imports; cannot finalize")
        struct.pack_into("<II", data, directory+8, 0, 0)
    for index in (4, 9, 12, 13, 14):
        require(not any(data[directory+8*index:directory+8*index+8]),
                "unexpected signature/TLS/IAT/delay-import/CLR directory")
    reloc, reloc_size = struct.unpack_from("<II", data, directory+5*8)
    require(reloc and reloc_size >= 12, "relocation directory missing")
    block = rva_bytes(reloc, reloc_size)
    require(struct.unpack_from("<I", block, 4)[0] <= len(block), "invalid relocation block")
    require(any((struct.unpack_from("<H", block, i)[0] >> 12) == 10
                for i in range(8, len(block)-1, 2)), "no DIR64 relocation")
    struct.pack_into("<I", data, opt+64, 0)  # unsigned EFI image checksum
    Path(destination).write_bytes(data)
    return dict(artifact=Path(destination).name, bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest(), machine="AMD64", subsystem=10,
                imports=0, tls=0, iat=0, relocation=True, nx=True, dynamic_base=True,
                hardware_tested=False, unlock_implemented=False, gpu_mmio="read-only" if live_prom else False,
                gpu_mmio_writes=False, pci_config_writes=False, dma=False,
                firmware_execution=False, mode="live-ROM-read-only-candidate" if live_prom else "cached-ROM-and-PCI-read-only")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    parser.add_argument("--report", required=True)
    parser.add_argument("--live-prom", action="store_true")
    args = parser.parse_args()
    result = verify(args.source, args.destination, args.live_prom)
    Path(args.report).write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(result))
