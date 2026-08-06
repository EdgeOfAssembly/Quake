#!/usr/bin/env python3
"""Convert Quake1 alias .mdl (chosen frame) + palette.lmp → OBJ + PNG skin.

References:
  - https://quakewiki.org/wiki/.pak
  - Unofficial Quake Specs §5 (Alias / IDPO models)
"""
from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


def read_palette(path: Path) -> list[tuple[int, int, int]]:
    raw = path.read_bytes()
    if len(raw) < 768:
        raise SystemExit(f"palette too small: {path}")
    return [(raw[i], raw[i + 1], raw[i + 2]) for i in range(0, 768, 3)]


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    """Minimal PNG writer (RGBA8)."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw_rows = b"".join(
        b"\x00" + rgba[y * width * 4 : (y + 1) * width * 4] for y in range(height)
    )
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw_rows, 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def indices_to_rgba(idx: bytes, pal: list[tuple[int, int, int]]) -> bytes:
    rgba = bytearray(len(idx) * 4)
    for i, pix in enumerate(idx):
        r, g, b = pal[pix]
        a = 0 if pix == 255 else 255
        o = i * 4
        rgba[o : o + 4] = bytes((r, g, b, a))
    return bytes(rgba)


def convert(mdl_path: Path, palette_path: Path, out_dir: Path, frame_index: int = 0) -> None:
    data = mdl_path.read_bytes()
    if len(data) < 84 or data[0:4] != b"IDPO":
        raise SystemExit(f"not an IDPO mdl: {mdl_path}")
    version = struct.unpack_from("<i", data, 4)[0]
    if version != 6:
        raise SystemExit(f"unsupported mdl version {version}")

    scale = struct.unpack_from("<fff", data, 8)
    origin = struct.unpack_from("<fff", data, 20)
    numskins, skinwidth, skinheight, numverts, numtris, numframes = struct.unpack_from(
        "<iiiiii", data, 48
    )

    pos = 84
    pal = read_palette(palette_path)
    skins_rgba: list[bytes] = []

    for _ in range(numskins):
        group = struct.unpack_from("<i", data, pos)[0]
        pos += 4
        n = skinwidth * skinheight
        if group == 0:
            idx = data[pos : pos + n]
            pos += n
            skins_rgba.append(indices_to_rgba(idx, pal))
        else:
            nb = struct.unpack_from("<i", data, pos)[0]
            pos += 4
            pos += 4 * nb  # times
            # first picture of group
            idx = data[pos : pos + n]
            pos += n * nb
            skins_rgba.append(indices_to_rgba(idx, pal))

    stverts: list[tuple[int, int, int]] = []
    for _ in range(numverts):
        onseam, s, t = struct.unpack_from("<iii", data, pos)
        pos += 12
        stverts.append((onseam, s, t))

    tris: list[tuple[int, int, int, int]] = []
    for _ in range(numtris):
        facesfront, v0, v1, v2 = struct.unpack_from("<iiii", data, pos)
        pos += 16
        tris.append((facesfront, v0, v1, v2))

    def parse_simple_frame(p: int) -> tuple[int, str, list[tuple[float, float, float]]]:
        p += 8  # min/max trivertx
        name_b = data[p : p + 16]
        p += 16
        name = name_b.split(b"\0", 1)[0].decode("latin1", errors="replace")
        verts: list[tuple[float, float, float]] = []
        for _vi in range(numverts):
            x, y, z = data[p], data[p + 1], data[p + 2]
            p += 4
            verts.append(
                (
                    x * scale[0] + origin[0],
                    y * scale[1] + origin[1],
                    z * scale[2] + origin[2],
                )
            )
        return p, name, verts

    frames: list[tuple[str, list[tuple[float, float, float]]]] = []
    p = pos
    for _fi in range(numframes):
        ftype = struct.unpack_from("<i", data, p)[0]
        p += 4
        if ftype == 0:
            p, name, verts = parse_simple_frame(p)
            frames.append((name, verts))
        else:
            n = struct.unpack_from("<i", data, p)[0]
            p += 4
            p += 8  # min max
            p += 4 * n  # times
            for _ in range(n):
                p, name, verts = parse_simple_frame(p)
                frames.append((name, verts))

    if not frames:
        raise SystemExit("no frames parsed")
    if frame_index < 0 or frame_index >= len(frames):
        frame_index = 0
    fname, verts = frames[frame_index]

    out_dir.mkdir(parents=True, exist_ok=True)
    stem = mdl_path.stem
    skin_path = out_dir / f"{stem}_skin.png"
    obj_path = out_dir / f"{stem}.obj"
    mtl_path = out_dir / f"{stem}.mtl"

    if not skins_rgba:
        raise SystemExit("no skins in mdl")
    write_png(skin_path, skinwidth, skinheight, skins_rgba[0])

    lines: list[str] = [
        f"# Quake MDL {mdl_path.name} frame={frame_index} name={fname}",
        f"mtllib {mtl_path.name}",
        f"usemtl {stem}_skin",
    ]
    for x, y, z in verts:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")

    uv_i = 1
    face_lines: list[str] = []
    for facesfront, i0, i1, i2 in tris:
        uvs: list[int] = []
        for vi in (i0, i1, i2):
            onseam, s, t = stverts[vi]
            sf = float(s)
            if facesfront == 0 and onseam:
                sf += skinwidth * 0.5
            u = sf / float(skinwidth)
            v = 1.0 - (float(t) / float(skinheight))
            lines.append(f"vt {u:.6f} {v:.6f}")
            uvs.append(uv_i)
            uv_i += 1
        face_lines.append(f"f {i0 + 1}/{uvs[0]} {i1 + 1}/{uvs[1]} {i2 + 1}/{uvs[2]}")

    lines.extend(face_lines)
    obj_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    mtl_path.write_text(
        f"newmtl {stem}_skin\nKa 1 1 1\nKd 1 1 1\nmap_Kd {skin_path.name}\n",
        encoding="utf-8",
    )

    print(f"wrote {obj_path}", flush=True)
    print(f"wrote {skin_path} ({skinwidth}x{skinheight})", flush=True)
    print(f"wrote {mtl_path}", flush=True)
    print(
        f"frame={frame_index} name={fname} verts={len(verts)} tris={len(tris)} "
        f"total_frames={len(frames)}",
        flush=True,
    )


def main() -> None:
    ap = argparse.ArgumentParser(description="Quake1 MDL → OBJ+PNG")
    ap.add_argument("mdl", type=Path)
    ap.add_argument("palette", type=Path, help="gfx/palette.lmp")
    ap.add_argument("-o", "--out-dir", type=Path, required=True)
    ap.add_argument("-f", "--frame", type=int, default=0)
    args = ap.parse_args()
    convert(args.mdl, args.palette, args.out_dir, frame_index=args.frame)


if __name__ == "__main__":
    main()
