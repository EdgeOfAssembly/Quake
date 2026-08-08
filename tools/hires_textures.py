#!/usr/bin/env python3
"""Build hi-res Quake texture packs for software quake.x11.

Pipeline:
  1. Parse BSP texture lump (or raw .mip)
  2. Export full-res mip0 as PNG (Quake palette)
  3. Upscale with **xBRZ** or **Real-ESRGAN** (×2–×4)
  4. Write textures/<name>.tga (truecolor) + .mip (8-bit fallback)
  5. Install under -game dirs: hires_xbrz / hires_esr

Usage:
  python3 tools/hires_textures.py extract-bsp maps/e1m1.bsp -o /tmp/tex-src
  python3 tools/hires_textures.py upscale /tmp/tex-src /tmp/tex-x4 -s 4 --backend xbrz
  python3 tools/hires_textures.py pack /tmp/tex-x4 hires_xbrz/textures --palette gfx/palette.lmp
  # dual packs (recommended):
  python3 tools/hires_textures.py build-dual --scale 4
  # single:
  python3 tools/hires_textures.py build-e1m1 --backend xbrz --game hires_xbrz --scale 4
  python3 tools/hires_textures.py build-e1m1 --backend esrgan --game hires_esr --scale 4

Run:
  ./quake.x11 -basedir . -game hires_xbrz -mem 512 ...
  ./quake.x11 -basedir . -game hires_esr  -mem 512 ...
  # later -game overrides earlier if stacked
"""
from __future__ import annotations

import argparse
import shutil
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


# Quake software: 224–254 are fullbright ramps; 255 often unused/transparent.
# Mapping AI-upscaled bright RGB into those indices washes walls to white.
FULLBRIGHT_LO = 224
FULLBRIGHT_HI = 255  # exclusive end for "avoid" set → use 0..223 only by default


def _palette_allowed(
    pal: list[tuple[int, int, int]],
    prefer_indices: set[int] | None,
    allow_fullbright: bool,
):
    import numpy as np

    if prefer_indices:
        idx = sorted(i for i in prefer_indices if 0 <= i < 256)
        if not allow_fullbright:
            idx = [i for i in idx if i < FULLBRIGHT_LO]
        if not idx:
            idx = list(range(FULLBRIGHT_LO if allow_fullbright else 0,
                             256 if allow_fullbright else FULLBRIGHT_LO))
    else:
        if allow_fullbright:
            idx = list(range(256))
        else:
            idx = list(range(FULLBRIGHT_LO))  # 0..223
    return np.array(idx, dtype=np.int32)


def quantize_rgb_to_quake(
    arr,
    pal: list[tuple[int, int, int]],
    *,
    prefer_indices: set[int] | None = None,
    allow_fullbright: bool = False,
    dither: bool = True,
) -> bytes:
    """Map HxWx3 uint8 RGB → Quake palette indices with optional FS dither.

    Uses perceptual weights and by default **excludes fullbright 224–255**,
    which is what made the first hires pack look blown-out white.
    """
    import numpy as np

    h, w, _ = arr.shape
    pal_a = np.array(pal, dtype=np.float64)
    allowed = _palette_allowed(pal, prefer_indices, allow_fullbright)
    pal_sub = pal_a[allowed]  # M x 3

    # perceptual weights (rough Rec.601)
    wt = np.array([0.299, 0.587, 0.114], dtype=np.float64)

    work = arr.astype(np.float64).copy()
    out = np.empty((h, w), dtype=np.uint8)

    def nearest(rgb: np.ndarray) -> int:
        # rgb shape (3,)
        d = pal_sub - rgb[None, :]
        dist = np.sum(wt[None, :] * (d * d), axis=1)
        return int(allowed[int(np.argmin(dist))])

    if not dither:
        flat = work.reshape(-1, 3)
        # vectorized nearest
        # dist[n,m] = sum wt*(flat[n]-pal_sub[m])^2
        chunk = 2048
        o = np.empty(flat.shape[0], dtype=np.uint8)
        for i in range(0, flat.shape[0], chunk):
            block = flat[i : i + chunk]
            d = block[:, None, :] - pal_sub[None, :, :]
            dist = np.sum(wt[None, None, :] * (d * d), axis=2)
            o[i : i + chunk] = allowed[np.argmin(dist, axis=1)].astype(np.uint8)
        return o.tobytes()

    # Floyd–Steinberg dithering
    for y in range(h):
        for x in range(w):
            old = work[y, x].copy()
            old = np.clip(old, 0, 255)
            idx = nearest(old)
            out[y, x] = idx
            new = pal_a[idx]
            err = old - new
            if x + 1 < w:
                work[y, x + 1] += err * (7.0 / 16.0)
            if y + 1 < h:
                if x > 0:
                    work[y + 1, x - 1] += err * (3.0 / 16.0)
                work[y + 1, x] += err * (5.0 / 16.0)
                if x + 1 < w:
                    work[y + 1, x + 1] += err * (1.0 / 16.0)
    return out.tobytes()


def indices_from_png_file(path: Path, pal: list[tuple[int, int, int]]) -> set[int]:
    """Unique palette indices used by an already-indexed or RGB original PNG."""
    from PIL import Image
    import numpy as np

    img = Image.open(path)
    if img.mode == "P":
        return set(int(i) for i in img.getdata())
    img = img.convert("RGB")
    arr = np.asarray(img, dtype=np.uint8)
    raw = quantize_rgb_to_quake(arr, pal, allow_fullbright=True, dither=False)
    return set(raw)


def png_to_indices(
    path: Path,
    pal: list[tuple[int, int, int]],
    *,
    original_png: Path | None = None,
) -> tuple[bytes, int, int]:
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
    arr = np.asarray(img, dtype=np.uint8)

    prefer: set[int] | None = None
    allow_fb = False
    if original_png is not None and original_png.is_file():
        prefer = indices_from_png_file(original_png, pal)
        # only allow fullbright if original used them
        allow_fb = any(i >= FULLBRIGHT_LO for i in prefer)
        # expand preferred set with a few neighbors by RGB proximity (helps upscale)
        prefer = _expand_palette_set(prefer, pal, extra=24)

    data = quantize_rgb_to_quake(
        arr, pal, prefer_indices=prefer, allow_fullbright=allow_fb, dither=True
    )
    return data, w, h


def _expand_palette_set(
    base: set[int], pal: list[tuple[int, int, int]], extra: int
) -> set[int]:
    """Add `extra` nearest non-fullbright colors to the set used by the original."""
    import numpy as np

    if not base:
        return set(range(FULLBRIGHT_LO))
    pal_a = np.array(pal, dtype=np.float64)
    wt = np.array([0.299, 0.587, 0.114])
    used = np.array(sorted(i for i in base if i < FULLBRIGHT_LO), dtype=np.int32)
    if used.size == 0:
        used = np.arange(FULLBRIGHT_LO)
    # mean color of original
    mean = pal_a[used].mean(axis=0)
    candidates = []
    for i in range(FULLBRIGHT_LO):
        if i in base:
            continue
        d = pal_a[i] - mean
        candidates.append((float(np.sum(wt * d * d)), i))
    candidates.sort()
    out = set(int(x) for x in used)
    for _, i in candidates[:extra]:
        out.add(i)
    return out


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


def write_tga_topdown_rgb(png: Path, tga: Path) -> tuple[int, int]:
    """Write 32-bit top-left TGA for engine truecolor path (Image_LoadTGA)."""
    from PIL import Image

    img = Image.open(png).convert("RGBA")
    w, h = img.size
    # 16-align (engine prefers); pad by crop if needed
    w2, h2 = (w // 16) * 16, (h // 16) * 16
    if w2 < 16 or h2 < 16:
        raise ValueError(f"{png}: size {w}x{h} too small after 16-align")
    if (w2, h2) != (w, h):
        img = img.resize((w2, h2), Image.Resampling.LANCZOS)
        w, h = w2, h2
    pixels = img.load()
    body = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            body.extend((b, g, r, 255))
    header = bytearray(18)
    header[2] = 2
    header[12] = w & 0xFF
    header[13] = (w >> 8) & 0xFF
    header[14] = h & 0xFF
    header[15] = (h >> 8) & 0xFF
    header[16] = 32
    header[17] = 0x20  # top-left
    tga.parent.mkdir(parents=True, exist_ok=True)
    tga.write_bytes(bytes(header) + body)
    return w, h


def upscale_dir(
    src: Path,
    dst: Path,
    scale: int,
    model: str,
    *,
    backend: str = "esrgan",
    tta: bool = False,
    tile: int = 200,
    gpu: int | None = 1,
) -> None:
    """Upscale to the size the game should use — do NOT downscale afterward.

    Engine will fit to 16-aligned <= 1024 if needed (Image_FitTextureSize).

    backend:
      esrgan — realesrgan-ncnn-vulkan (photo-like)
      xbrz   — xbrzscale (pixel-art, scale 2..6)
    """
    dst.mkdir(parents=True, exist_ok=True)
    pngs = sorted(src.glob("*.png"))
    if not pngs:
        raise SystemExit(f"no PNGs in {src}")

    backend = backend.lower().strip()
    if backend not in ("esrgan", "xbrz"):
        raise SystemExit(f"unknown backend {backend!r} (use esrgan|xbrz)")

    if backend == "xbrz":
        if scale < 2 or scale > 6:
            raise SystemExit(f"xbrz scale must be 2..6, got {scale}")
        xbrz = shutil.which("xbrzscale")
        if not xbrz:
            raise SystemExit("xbrzscale not found in PATH")
        for png in pngs:
            out = dst / png.name
            cmd = [xbrz, str(scale), str(png), str(out)]
            print("run:", " ".join(cmd), flush=True)
            subprocess.check_call(cmd)
    else:
        models = Path("/usr/share/realesrgan-ncnn-vulkan/models")
        if not models.is_dir():
            models = Path.home() / ".local/share/realesrgan-ncnn-vulkan/models"
        for png in pngs:
            out = dst / png.name
            cmd = [
                "realesrgan-ncnn-vulkan",
                "-i",
                str(png),
                "-o",
                str(out),
                "-s",
                str(scale),
                "-n",
                model,
                "-f",
                "png",
                "-t",
                str(tile),
            ]
            if models.is_dir():
                cmd.extend(["-m", str(models)])
            if tta:
                cmd.append("-x")
            if gpu is not None:
                cmd.extend(["-g", str(gpu)])
            print("run:", " ".join(cmd), flush=True)
            subprocess.check_call(cmd)

    for p in src.glob("*.name"):
        (dst / p.name).write_text(p.read_text())


def pack_dir(
    src: Path,
    out_tex: Path,
    pal: list[tuple[int, int, int]],
    *,
    original_dir: Path | None = None,
    write_tga: bool = True,
) -> int:
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
        # skip clip/trigger (invisible) and sky (engine layout)
        if qname in ("clip", "trigger") or qname.startswith("sky"):
            print(f"skip {qname}")
            continue
        orig = None
        if original_dir is not None:
            cand = original_dir / f"{sanitize_filename(qname)}.png"
            if cand.is_file():
                orig = cand
        safe = sanitize_filename(qname)
        if write_tga:
            tw, th = write_tga_topdown_rgb(png, out_tex / f"{safe}.tga")
            print(f"tga  {qname} {tw}x{th} -> {out_tex / (safe + '.tga')}")
        indices, w, h = png_to_indices(png, pal, original_png=orig)
        pixels = build_mips(indices, w, h)
        out = out_tex / (safe + ".mip")
        write_mip(out, qname, w, h, pixels)
        print(f"pack {qname} {w}x{h} -> {out} (fb={'yes' if any(b>=FULLBRIGHT_LO for b in indices) else 'no'})")
        n += 1
    return n


def _write_pack_readme(game_dir: Path, backend: str, scale: int) -> None:
    game_dir.mkdir(parents=True, exist_ok=True)
    text = f"""# {game_dir.name} — Quake hi-res pack ({backend} ×{scale})

## Run

```bash
./quake.x11 -basedir . -game {game_dir.name} -mem 512 -width 1280 -height 720 -window +map e1m1
```

## Backend

- **xbrz** — pixel-art scaler (sharp, on-palette, safe for water)
- **esrgan** — Real-ESRGAN neural (softer, more detail; color can drift)

Stack packs (later wins):

```bash
./quake.x11 -basedir . -game hires_esr -game hires_xbrz ...
```

Rebuild: `python3 tools/hires_textures.py build-dual --scale {scale}`
"""
    (game_dir / "README.md").write_text(text)


def cmd_build_e1m1(args: argparse.Namespace) -> None:
    root = Path(args.quake_root).resolve()
    pak = root / args.pak
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    qpak = Path(args.qpak)
    backend = getattr(args, "backend", "esrgan")
    game = getattr(args, "game", None)
    if not game:
        game = "hires_xbrz" if backend == "xbrz" else "hires_esr"

    # extract
    subprocess.check_call(
        [str(qpak), "extract", str(pak), str(work / "extract"), "--exact", "gfx/palette.lmp"]
    )
    subprocess.check_call(
        [str(qpak), "extract", str(pak), str(work / "extract"), "--exact", "maps/e1m1.bsp"]
    )
    pal = load_palette(work / "extract/gfx/palette.lmp")

    game_dir = root / game
    (game_dir / "gfx").mkdir(parents=True, exist_ok=True)
    (game_dir / "gfx" / "palette.lmp").write_bytes((work / "extract/gfx/palette.lmp").read_bytes())

    src = work / "png"
    extract_bsp_textures(work / "extract/maps/e1m1.bsp", src, pal)

    tag = f"{backend}_x{args.scale}{'tta' if getattr(args, 'tta', False) else ''}"
    up = work / f"png_{tag}"
    upscale_dir(
        src,
        up,
        args.scale,
        getattr(args, "model", "realesrgan-x4plus"),
        backend=backend,
        tta=getattr(args, "tta", False),
    )
    n = pack_dir(up, game_dir / "textures", pal, original_dir=src, write_tga=True)
    _write_pack_readme(game_dir, backend, args.scale)
    print(f"done: {n} textures in {game_dir / 'textures'}")
    print(f"run: ./quake.x11 -basedir . -game {game} -mem 512 -width 1280 -height 720 -window +map e1m1")


def cmd_build_dual(args: argparse.Namespace) -> None:
    """Build both hires_xbrz and hires_esr from the same e1m1 extract."""
    root = Path(args.quake_root).resolve()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    qpak = Path(args.qpak)
    pak = root / args.pak

    subprocess.check_call(
        [str(qpak), "extract", str(pak), str(work / "extract"), "--exact", "gfx/palette.lmp"]
    )
    subprocess.check_call(
        [str(qpak), "extract", str(pak), str(work / "extract"), "--exact", "maps/e1m1.bsp"]
    )
    pal = load_palette(work / "extract/gfx/palette.lmp")
    src = work / "png"
    extract_bsp_textures(work / "extract/maps/e1m1.bsp", src, pal)

    for backend, game in (("xbrz", "hires_xbrz"), ("esrgan", "hires_esr")):
        print(f"\n======== building {game} ({backend} ×{args.scale}) ========\n", flush=True)
        ns = argparse.Namespace(
            quake_root=str(root),
            pak=args.pak,
            qpak=args.qpak,
            work=str(work),
            scale=args.scale,
            model=args.model,
            tta=args.tta if backend == "esrgan" else False,
            backend=backend,
            game=game,
        )
        # reuse extract; force extract skip by leaving png in place
        game_dir = root / game
        (game_dir / "gfx").mkdir(parents=True, exist_ok=True)
        (game_dir / "gfx" / "palette.lmp").write_bytes(
            (work / "extract/gfx/palette.lmp").read_bytes()
        )
        tag = f"{backend}_x{args.scale}{'tta' if ns.tta else ''}"
        up = work / f"png_{tag}"
        upscale_dir(
            src,
            up,
            args.scale,
            args.model,
            backend=backend,
            tta=ns.tta,
        )
        n = pack_dir(up, game_dir / "textures", pal, original_dir=src, write_tga=True)
        _write_pack_readme(game_dir, backend, args.scale)
        print(f"done {game}: {n} textures")

    print(
        "\nBoth packs ready:\n"
        "  ./quake.x11 -basedir . -game hires_xbrz -mem 512 ...\n"
        "  ./quake.x11 -basedir . -game hires_esr  -mem 512 ...\n"
    )


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
    p.add_argument("-s", type=int, default=4)
    p.add_argument("-n", default="realesrgan-x4plus")
    p.add_argument(
        "--backend",
        choices=("esrgan", "xbrz"),
        default="esrgan",
        help="Upscaler: esrgan (neural) or xbrz (pixel-art)",
    )

    p = sub.add_parser("pack")
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--palette", required=True)
    p.add_argument(
        "--original",
        default=None,
        help="Dir of pre-upscale PNGs (constrains palette to original colors)",
    )
    p.add_argument("--no-tga", action="store_true", help="Skip writing .tga (mip only)")

    p = sub.add_parser("build-e1m1")
    p.add_argument("--quake-root", default=".")
    p.add_argument("--pak", default="id1/pak0.pak")
    p.add_argument("--qpak", default="/mnt/qpaktool/build/release/qpak")
    p.add_argument("--work", default="/tmp/quake-hires-work")
    p.add_argument("--scale", type=int, default=4,
                   help="Upscale factor (default 4)")
    p.add_argument("--model", default="realesrgan-x4plus")
    p.add_argument("--tta", action="store_true",
                   help="Real-ESRGAN -x TTA mode (slower, better quality)")
    p.add_argument(
        "--backend",
        choices=("esrgan", "xbrz"),
        default="xbrz",
        help="Upscaler backend (default xbrz)",
    )
    p.add_argument(
        "--game",
        default=None,
        help="Output gamedir name (default hires_xbrz or hires_esr)",
    )

    p = sub.add_parser(
        "build-dual",
        help="Build both hires_xbrz and hires_esr packs from e1m1",
    )
    p.add_argument("--quake-root", default=".")
    p.add_argument("--pak", default="id1/pak0.pak")
    p.add_argument("--qpak", default="/mnt/qpaktool/build/release/qpak")
    p.add_argument("--work", default="/tmp/quake-hires-work")
    p.add_argument("--scale", type=int, default=4)
    p.add_argument("--model", default="realesrgan-x4plus")
    p.add_argument("--tta", action="store_true", help="TTA for ESRGAN pack only")

    args = ap.parse_args()
    if args.cmd == "extract-bsp":
        pal = load_palette(Path(args.palette))
        n = extract_bsp_textures(Path(args.bsp), Path(args.o), pal)
        print(f"exported {n}")
    elif args.cmd == "upscale":
        upscale_dir(
            Path(args.src),
            Path(args.dst),
            args.s,
            args.n,
            backend=args.backend,
        )
    elif args.cmd == "pack":
        pal = load_palette(Path(args.palette))
        orig = Path(args.original) if getattr(args, "original", None) else None
        n = pack_dir(
            Path(args.src),
            Path(args.dst),
            pal,
            original_dir=orig,
            write_tga=not args.no_tga,
        )
        print(f"packed {n}")
    elif args.cmd == "build-e1m1":
        cmd_build_e1m1(args)
    elif args.cmd == "build-dual":
        cmd_build_dual(args)


if __name__ == "__main__":
    main()
