#!/usr/bin/env python3
"""Upscale Quake gfx.wad qpic lumps (xBRZ/ESRGAN) into a gamedir.

Lumps are stored 4× larger; software HUD uses Draw_GuiScale + PicFit so
pre-scaled art still lands at the right on-screen size.

Usage:
  python3 tools/hires_gfx.py build --backend xbrz --scale 4 --game hires_xbrz
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

# Quake WAD2 lump type for qpic
TYP_QPIC = 0x40
TYP_PALETTE = 0x40  # some tools use same; palette is special name


def load_palette(path: Path) -> list[tuple[int, int, int]]:
    data = path.read_bytes()
    return [(data[i], data[i + 1], data[i + 2]) for i in range(0, 768, 3)]


def quantize(img, pal, allow_fb: bool = False) -> bytes:
    import numpy as np
    from PIL import Image

    if not isinstance(img, Image.Image):
        img = Image.fromarray(img, "RGB")
    arr = np.asarray(img.convert("RGB"), dtype=np.int16)
    use = list(range(0, 224 if not allow_fb else 256))
    pals = np.array([pal[i] for i in use], dtype=np.int16)
    flat = arr.reshape(-1, 3)
    out = np.empty(flat.shape[0], dtype=np.uint8)
    step = 8192
    for i in range(0, flat.shape[0], step):
        chunk = flat[i : i + step]
        d = ((chunk[:, None, :] - pals[None, :, :]) ** 2).sum(axis=2)
        out[i : i + step] = np.array(use, dtype=np.uint8)[d.argmin(axis=1)]
    return out.tobytes()


def upscale(src: Path, dst: Path, scale: int, backend: str) -> None:
    if backend == "xbrz":
        subprocess.check_call(["xbrzscale", str(scale), str(src), str(dst)])
    else:
        models = Path("/usr/share/realesrgan-ncnn-vulkan/models")
        cmd = [
            "realesrgan-ncnn-vulkan",
            "-i",
            str(src),
            "-o",
            str(dst),
            "-s",
            str(scale),
            "-n",
            "realesrgan-x4plus",
            "-f",
            "png",
        ]
        if models.is_dir():
            cmd.extend(["-m", str(models)])
        subprocess.check_call(cmd)


def parse_wad2(data: bytes) -> list[tuple[str, int, int, int, bytes]]:
    """Return list of (name, type, filepos, size, payload)."""
    if data[:4] != b"WAD2":
        raise ValueError("not WAD2")
    num, diroff = struct.unpack_from("<ii", data, 4)
    lumps = []
    for i in range(num):
        e = data[diroff + i * 32 : diroff + i * 32 + 32]
        # filepos, disksize, size, type, compression, pad1, pad2, name[16]
        filepos, disksize, size, typ, compression, pad1, pad2 = struct.unpack_from(
            "<iiibbbb", e, 0
        )
        name = e[16:32].split(b"\0")[0].decode("latin1")
        payload = data[filepos : filepos + size]
        lumps.append((name, typ, filepos, size, payload))
    return lumps


def build_wad2(lumps: list[tuple[str, int, bytes]]) -> bytes:
    """lumps: (name, type, payload)"""
    body = bytearray()
    entries = []
    for name, typ, payload in lumps:
        filepos = 12 + len(body)
        body.extend(payload)
        while len(body) & 3:
            body.append(0)
        nm = name.encode("latin1")[:15]
        nm = nm + b"\0" * (16 - len(nm))
        entries.append((filepos, len(payload), len(payload), typ, nm))
    diroff = 12 + len(body)
    directory = bytearray()
    for filepos, disksize, size, typ, nm in entries:
        directory.extend(struct.pack("<iiibbbb", filepos, disksize, size, typ, 0, 0, 0))
        directory.extend(nm)
    return b"WAD2" + struct.pack("<ii", len(entries), diroff) + bytes(body) + bytes(directory)


def cmd_build(args: argparse.Namespace) -> None:
    from PIL import Image

    root = Path(args.quake_root).resolve()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    qpak = Path(args.qpak)
    game = root / args.game
    game.mkdir(parents=True, exist_ok=True)

    subprocess.check_call(
        [str(qpak), "extract", str(root / "id1/pak0.pak"), str(work / "ex"), "--exact", "gfx.wad"]
    )
    subprocess.check_call(
        [
            str(qpak),
            "extract",
            str(root / "id1/pak0.pak"),
            str(work / "ex"),
            "--exact",
            "gfx/palette.lmp",
        ]
    )
    pal = load_palette(work / "ex/gfx/palette.lmp")
    (game / "gfx").mkdir(parents=True, exist_ok=True)
    (game / "gfx" / "palette.lmp").write_bytes((work / "ex/gfx/palette.lmp").read_bytes())

    wad_path = work / "ex/gfx.wad"
    data = wad_path.read_bytes()
    lumps_in = parse_wad2(data)
    out_lumps: list[tuple[str, int, bytes]] = []
    img_dir = work / "gfx_img"
    img_dir.mkdir(exist_ok=True)

    for name, typ, _fp, _sz, payload in lumps_in:
        # Quake gfx.wad: type 66 (0x42) = qpic; 68 (0x44) = CONCHARS raw 128²
        if name.upper() == "CONCHARS" and len(payload) == 128 * 128:
            # font sheet 128x128
            w = h = 128
            img = Image.new("RGB", (w, h))
            px = img.load()
            for y in range(h):
                for x in range(w):
                    px[x, y] = pal[payload[y * w + x]]
            sp = img_dir / "CONCHARS.png"
            dp = img_dir / f"CONCHARS_x{args.scale}.png"
            img.save(sp)
            try:
                upscale(sp, dp, args.scale, args.backend)
                up = Image.open(dp).convert("RGB")
                # engine expects 128x128 conchars — keep size, just enhance
                if up.size != (128, 128):
                    up = up.resize((128, 128), Image.Resampling.LANCZOS)
                indices = quantize(up, pal, allow_fb=False)
                out_lumps.append((name, typ, indices))
                print(f"gfx CONCHARS: enhanced {args.backend} (kept 128x128)")
                continue
            except Exception as e:
                print(f"gfx FAIL CONCHARS: {e}")
        elif len(payload) >= 8 and typ in (0x40, 0x42, 66):
            w, h = struct.unpack_from("<ii", payload, 0)
            if w > 0 and h > 0 and w * h + 8 <= len(payload) and w < 2048 and h < 2048:
                pix = payload[8 : 8 + w * h]
                if len(pix) == w * h:
                    img = Image.new("RGB", (w, h))
                    px = img.load()
                    for y in range(h):
                        for x in range(w):
                            px[x, y] = pal[pix[y * w + x]]
                    sp = img_dir / f"{name}.png"
                    dp = img_dir / f"{name}_x{args.scale}.png"
                    img.save(sp)
                    try:
                        upscale(sp, dp, args.scale, args.backend)
                        up = Image.open(dp).convert("RGB")
                        indices = quantize(up, pal, allow_fb=False)
                        nw, nh = up.size
                        new_payload = struct.pack("<ii", nw, nh) + indices
                        out_lumps.append((name, typ, new_payload))
                        print(f"gfx {name}: {w}x{h} -> {nw}x{nh}")
                        continue
                    except Exception as e:
                        print(f"gfx FAIL {name}: {e}, keep original")
        out_lumps.append((name, typ, payload))

    wad_out = build_wad2(out_lumps)
    (game / "gfx.wad").write_bytes(wad_out)
    print(f"wrote {game / 'gfx.wad'} ({len(wad_out)} bytes, {len(out_lumps)} lumps)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("build")
    p.add_argument("--quake-root", default=".")
    p.add_argument("--qpak", default="/mnt/qpaktool/build/release/qpak")
    p.add_argument("--work", default="/tmp/quake-hires-gfx")
    p.add_argument("--backend", choices=("xbrz", "esrgan"), default="xbrz")
    p.add_argument("--scale", type=int, default=4)
    p.add_argument("--game", required=True)
    args = ap.parse_args()
    if args.cmd == "build":
        cmd_build(args)


if __name__ == "__main__":
    main()
