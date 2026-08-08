#!/usr/bin/env python3
"""Pack id1/maps/*.bsp into id1/pak2.pak (overrides stock maps for water VIS)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    maps_dir = root / "id1" / "maps"
    out_pak = root / "id1" / "pak2.pak"
    files = sorted(maps_dir.glob("*.bsp"))
    if not files:
        print("no maps in id1/maps", file=sys.stderr)
        return 1
    data = bytearray()
    entries: list[tuple[bytes, int, int]] = []
    for f in files:
        raw = f.read_bytes()
        name = f"maps/{f.name}".encode("ascii")
        name = name + b"\0" * (56 - len(name))
        off = len(data)
        data.extend(raw)
        entries.append((name, off, len(raw)))
        print(f"  {f.name}: {len(raw)}")
    diroff = 12 + len(data)
    directory = bytearray()
    for name, off, size in entries:
        directory.extend(name)
        directory.extend(struct.pack("<II", off, size))
    out_pak.write_bytes(b"PACK" + struct.pack("<II", diroff, len(entries) * 64) + data + directory)
    print(f"wrote {out_pak} ({out_pak.stat().st_size} bytes, {len(entries)} maps)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
