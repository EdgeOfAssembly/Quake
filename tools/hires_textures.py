#!/usr/bin/env python3
"""Build hi-res Quake miptex overrides for software quake.x11.

Pipeline:
  1. Parse BSP texture lump (or raw .mip)
  2. Export full-res mip0 as PNG (Quake palette)
  3. Upscale with realesrgan-ncnn-vulkan
  4. Quantize to Quake palette, rebuild 4 mips
  5. Write textures/<name>.mip for -game hires

Usage:
  python3 tools/hires_textures.py extract-bsp maps/e1m1.bsp -o /tmp/tex-src
  python3 tools/hires_textures.py upscale /tmp/tex-src /tmp/tex-x2 -s 2
  python3 tools/hires_textures.py pack /tmp/tex-x2 hires/textures --palette gfx/palette.lmp
  # or all-in-one:
  python3 tools/hires_textures.py build-e1m1 --pak id1/pak0.pak --scale 2
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

MIPLEVELS = 4
MIPTEX_HDR = 40  # sizeof miptex_t: name[16] + 2*u32 + 4*u32 offsets


def load_palette(path: Path) -> list[tuple[int, int, int]]:
    data = path.read_bytes()
    if len(data) < 768:
        raise SystemExit(f"bad palette: {path}")
    return [(data[i], data[i + 1], data[i + 2]) for i in range(0, 768, 3)]


def sanitize_filename(name: str) -> str:
    """Quake external texture convention: * → #."""
    return name.replace("*", "#")


def mip_pixels(w: int, h: int) -> int:
    """Total bytes for 4 mip levels (w*h * 85/64)."""
    return (w * h * 85) // 64


def build_mips(indices: bytes, w: int, h: int) -> bytes:
    """Box-filter mips from 8-bit indexed full image."""
    assert len(indices) == w * h
    levels = [indices]
    cw, ch = w, h
    for _ in range(1, MIPLEVELS):
        nw, nh = max(1, cw // 2), max(1, ch // 2)
        src = levels[-1]
        dst = bytearray(nw * nh)
        for y in range(nh):
            for x in range(nw):
                # sample 2x2 (or less at edges)
                x0, y0 = x * 2, y * 2
                x1, y1 = min(x0 + 1, cw - 1), min(y0 + 1, ch - 1)
                # pick top-left for indexed (box average of indices is wrong)
                dst[y * nw + x] = src[y0 * cw + x0]
        levels.append(bytes(dst))
        cw, ch = nw, nh
    return b"".join(levels)


def write_mip(path: Path, name: str, w: int, h: int, pixels: bytes) -> None:
    if (w & 15) or (h & 15):
        raise ValueError(f"{name}: size {w}x{h} not 16-aligned")
    need = mip_pixels(w, h)
    if len(pixels) < need:
        raise ValueError(f"{name}: need {need} pixels, got {len(pixels)}")
    pixels = pixels[:need]
    name_b = name.encode("latin1")[:15]
    name_b = name_b + b"\0" * (16 - len(name_b))
    offsets = []
    off = MIPTEX_HDR
    cw, ch = w, h
    for i in range(MIPLEVELS):
        offsets.append(off)
        off += cw * ch
        cw, ch = max(1, cw // 2), max(1, ch // 2)
    hdr = name_b + struct.pack("<II", w, h) + struct.pack("<4I", *offsets)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(hdr + pixels)


def indices_to_png(indices: bytes, w: int, h: int, pal: list[tuple[int, int, int]], out: Path) -> None:
    try:
        from PIL import Image
    except ImportError:
        # minimal PPM fallback then convert if possible
        raise SystemExit("Pillow required: pip install Pillow / use /mnt/python")
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = pal[indices[y * w + x]]
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)


def png_to_indices(path: Path, pal: list[tuple[int, int, int]]) -> tuple[bytes, int, int]:
    from PIL import Image
    import numpy as np

    img = Image.open(path).convert("RGB")
    w, h = img.size
    # snap to multiple of 16
    nw, nh = (w // 16) * 16, (h // 16) * 16
    if nw < 16 or nh < 16:
        raise ValueError(f"{path}: too small after align {w}x{h}")
    if (nw, nh) != (w, h):
        img = img.resize((nw, nh), Image.Resampling.LANCZOS)
        w, h = nw, nh
    arr = np.asarray(img, dtype=np.int16)
    pal_a = np.array(pal, dtype=np.int16)
    # nearest palette (chunked)
    flat = arr.reshape(-1, 3)
    out = np.empty(flat.shape[0], dtype=np.uint8)
    chunk = 4096
    for i in range(0, flat.shape[0], chunk):
        block = flat[i : i + chunk][:, None, :]  # N,1,3
        dist = np.sum((block - pal_a[None, :, :]) ** 2, axis=2)
        out[i : i + chunk] = np.argmin(dist, axis=1).astype(np.uint8)
    return out.tobytes(), w, h


def extract_bsp_textures(bsp_path: Path, out_dir: Path, pal: list[tuple[int, int, int]]) -> int:
    data = bsp_path.read_bytes()
    ver = struct.unpack_from("<I", data, 0)[0]
    if ver != 29:
        raise SystemExit(f"unsupported BSP version {ver}")
    ofs, length = struct.unpack_from("<ii", data, 4 + 2 * 8)
    num = struct.unpack_from("<i", data, ofs)[0]
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for i in range(num):
        dofs = struct.unpack_from("<i", data, ofs + 4 + i * 4)[0]
        if dofs < 0:
            continue
        base = ofs + dofs
        name = data[base : base + 16].split(b"\0")[0].decode("latin1")
        w, h = struct.unpack_from("<II", data, base + 16)
        off0 = struct.unpack_from("<I", data, base + 24)[0]
        # off0 is relative to miptex start
        pix = data[base + off0 : base + off0 + w * h]
        if len(pix) < w * h:
            print(f"skip {name}: short data", file=sys.stderr)
            continue
        fn = sanitize_filename(name) + ".png"
        indices_to_png(pix, w, h, pal, out_dir / fn)
        # also save meta
        (out_dir / (sanitize_filename(name) + ".name")).write_text(name)
        n += 1
        print(f"export {name} {w}x{h} -> {fn}")
    return n


def upscale_dir(src: Path, dst: Path, scale: int, model: str) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    models = Path("/usr/share/realesrgan-ncnn-vulkan/models")
    if not models.is_dir():
        models = Path.home() / ".local/share/realesrgan-ncnn-vulkan/models"
    cmd = [
        "realesrgan-ncnn-vulkan",
        "-i",
        str(src),
        "-o",
        str(dst),
        "-s",
        str(scale),
        "-n",
        model,
        "-f",
        "png",
    ]
    if models.is_dir():
        cmd.extend(["-m", str(models)])
    print("run:", " ".join(cmd), flush=True)
    subprocess.check_call(cmd)
    # copy .name sidecars
    for p in src.glob("*.name"):
        (dst / p.name).write_text(p.read_text())


def pack_dir(src: Path, out_tex: Path, pal: list[tuple[int, int, int]]) -> int:
    out_tex.mkdir(parents=True, exist_ok=True)
    n = 0
    # realesrgan may emit foo.png.png — accept *.png
    pngs = list(src.glob("*.png"))
    for png in sorted(pngs):
        # stem: strip trailing .png if double-extension
        stem = png.name
        while stem.lower().endswith(".png"):
            stem = stem[: -4]
        name_file = src / (stem + ".name")
        if not name_file.is_file():
            # try original stem before second .png
            name_file = src / (png.stem + ".name")
        qname = name_file.read_text().strip() if name_file.is_file() else stem.replace("#", "*")
        if qname.lower().endswith(".png"):
            qname = qname[: -4]
        # skip clip/trigger (invisible)
        if qname in ("clip", "trigger"):
            print(f"skip utility {qname}")
            continue
        indices, w, h = png_to_indices(png, pal)
        pixels = build_mips(indices, w, h)
        out = out_tex / (sanitize_filename(qname) + ".mip")
        write_mip(out, qname, w, h, pixels)
        print(f"pack {qname} {w}x{h} -> {out}")
        n += 1
    return n


def cmd_build_e1m1(args: argparse.Namespace) -> None:
    root = Path(args.quake_root)
    pak = root / args.pak
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    qpak = Path(args.qpak)
    # extract
    subprocess.check_call([str(qpak), "extract", str(pak), str(work / "extract"), "--exact", "gfx/palette.lmp"])
    subprocess.check_call([str(qpak), "extract", str(pak), str(work / "extract"), "--exact", "maps/e1m1.bsp"])
    pal = load_palette(work / "extract/gfx/palette.lmp")
    # also copy palette into hires for runtime if needed
    hires = root / "hires"
    (hires / "gfx").mkdir(parents=True, exist_ok=True)
    (hires / "gfx" / "palette.lmp").write_bytes((work / "extract/gfx/palette.lmp").read_bytes())

    src = work / "png"
    extract_bsp_textures(work / "extract/maps/e1m1.bsp", src, pal)
    up = work / f"png_x{args.scale}"
    upscale_dir(src, up, args.scale, args.model)
    n = pack_dir(up, hires / "textures", pal)
    print(f"done: {n} textures in {hires / 'textures'}")
    print(f"run: ./quake.x11 -basedir . -game hires -mem 256 -width 1920 -height 1080 -window +map e1m1")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract-bsp")
    p.add_argument("bsp")
    p.add_argument("-o", required=True)
    p.add_argument("--palette", default="id1/gfx/palette.lmp")

    p = sub.add_parser("upscale")
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("-s", type=int, default=2)
    p.add_argument("-n", default="realesrgan-x4plus")

    p = sub.add_parser("pack")
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--palette", required=True)

    p = sub.add_parser("build-e1m1")
    p.add_argument("--quake-root", default=".")
    p.add_argument("--pak", default="id1/pak0.pak")
    p.add_argument("--qpak", default="/mnt/qpaktool/build/release/qpak")
    p.add_argument("--work", default="/tmp/quake-hires-work")
    p.add_argument("--scale", type=int, default=2)
    p.add_argument("--model", default="realesrgan-x4plus")

    args = ap.parse_args()
    if args.cmd == "extract-bsp":
        pal = load_palette(Path(args.palette))
        n = extract_bsp_textures(Path(args.bsp), Path(args.o), pal)
        print(f"exported {n}")
    elif args.cmd == "upscale":
        upscale_dir(Path(args.src), Path(args.dst), args.s, args.n)
    elif args.cmd == "pack":
        pal = load_palette(Path(args.palette))
        n = pack_dir(Path(args.src), Path(args.dst), pal)
        print(f"packed {n}")
    elif args.cmd == "build-e1m1":
        cmd_build_e1m1(args)


if __name__ == "__main__":
    main()
