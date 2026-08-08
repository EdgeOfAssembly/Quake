#!/usr/bin/env python3
"""Rebuild ×4 turb (#water/#slime/#teleport) TGAs with color-preserving upscale."""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageEnhance


def load_palette(path: Path) -> list[tuple[int, int, int]]:
    data = path.read_bytes()
    return [(data[i], data[i + 1], data[i + 2]) for i in range(0, 768, 3)]


def extract_from_bsp(bsp: Path, palette: list[tuple[int, int, int]], out: Path) -> list[str]:
    raw = bsp.read_bytes()
    moff, _ = struct.unpack_from("<ii", raw, 4 + 2 * 8)
    num = struct.unpack_from("<i", raw, moff)[0]
    names: list[str] = []
    for i in range(num):
        toff = struct.unpack_from("<i", raw, moff + 4 + i * 4)[0]
        if toff < 0:
            continue
        base = moff + toff
        name = raw[base : base + 16].split(b"\0")[0]
        if not name.startswith(b"*"):
            continue
        if not (name.startswith(b"*water") or name.startswith(b"*slime") or name.startswith(b"*tele")):
            continue
        w, h = struct.unpack_from("<II", raw, base + 16)
        o0 = struct.unpack_from("<I", raw, base + 24)[0]
        pix = raw[base + o0 : base + o0 + w * h]
        img = Image.new("RGB", (w, h))
        px = img.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = palette[pix[y * w + x]]
        fn = name.decode("latin1").replace("*", "#")
        img.save(out / f"{fn}.png")
        names.append(fn)
        print(f"extract {fn} {w}x{h}")
    return names


def write_tga_topdown(img: Image.Image, path: Path) -> None:
    img = img.convert("RGBA")
    w, h = img.size
    body = bytearray()
    pixels = img.load()
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
    path.write_bytes(bytes(header) + body)
    print(f"wrote {path} {w}x{h}")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    bsp = root / "id1" / "maps" / "e1m1.bak"
    if not bsp.exists():
        bsp = root / "id1" / "maps" / "e1m1.bsp"
    pal_path = root / "hires" / "gfx" / "palette.lmp"
    if not pal_path.exists():
        pal_path = root / "id1" / "gfx" / "palette.lmp"
    out_tex = root / "hires" / "textures"
    work = Path("/tmp/turb-rebuild")
    work.mkdir(exist_ok=True)
    palette = load_palette(pal_path)
    names = extract_from_bsp(bsp, palette, work)
    for fn in names:
        lo = Image.open(work / f"{fn}.png").convert("RGB")
        hi = lo.resize((lo.width * 4, lo.height * 4), Image.Resampling.LANCZOS)
        hi = ImageEnhance.Color(hi).enhance(1.15)
        hi = ImageEnhance.Contrast(hi).enhance(1.08)
        # optional ESRGAN blend
        esr = work / f"{fn}_esr.png"
        try:
            lo.save(work / f"{fn}_src.png")
            r = subprocess.run(
                [
                    "realesrgan-ncnn-vulkan",
                    "-i",
                    str(work / f"{fn}_src.png"),
                    "-o",
                    str(esr),
                    "-s",
                    "4",
                    "-n",
                    "realesrgan-x4plus",
                ],
                capture_output=True,
                text=True,
                timeout=180,
            )
            if r.returncode == 0 and esr.exists():
                import numpy as np

                e = Image.open(esr).convert("RGB").resize(hi.size, Image.Resampling.LANCZOS)
                # color-match ESRGAN to original
                ha = np.asarray(hi, dtype=np.float32)
                ea = np.asarray(e, dtype=np.float32)
                la = np.asarray(lo.resize(hi.size, Image.Resampling.BILINEAR), dtype=np.float32)
                for c in range(3):
                    em, es = ea[:, :, c].mean(), ea[:, :, c].std() + 1e-6
                    lm, ls = la[:, :, c].mean(), la[:, :, c].std() + 1e-6
                    ea[:, :, c] = (ea[:, :, c] - em) / es * ls + lm
                e = Image.fromarray(np.clip(ea, 0, 255).astype(np.uint8), "RGB")
                hi = Image.blend(hi, e, 0.4)
                print(f"  ESRGAN blend {fn}")
        except Exception as ex:
            print(f"  ESRGAN skip {fn}: {ex}")
        write_tga_topdown(hi, out_tex / f"{fn}.tga")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
