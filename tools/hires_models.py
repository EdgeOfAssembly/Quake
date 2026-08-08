#!/usr/bin/env python3
"""Upscale Quake MDL skins (xBRZ or Real-ESRGAN) and rewrite progs/*.mdl.

Skin height must stay <= MAX_LBM_HEIGHT (engine: 2048 after our raise).
Default scale 2 is always safe; scale 4 needs engine MAX_LBM_HEIGHT >= ~800.

Usage:
  python3 tools/hires_models.py build --backend xbrz --scale 2 --game hires_xbrz
  python3 tools/hires_models.py build --backend esrgan --scale 2 --game hires_esr
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

ALIAS_VERSION = 6
ALIAS_SKIN_SINGLE = 0
ALIAS_SKIN_GROUP = 1
IDPO = b"IDPO"

# mdl_t after ident+version
# scale[3]f, scale_origin[3]f, boundingradius f, eyeposition[3]f,
# numskins, skinwidth, skinheight, numverts, numtris, numframes, synctype, flags, size f
HDR_AFTER_ID = 4 + 4  # ident already read as 4s, version i
# Full header size
MDL_HDR_SIZE = 84


def load_palette(path: Path) -> list[tuple[int, int, int]]:
    data = path.read_bytes()
    return [(data[i], data[i + 1], data[i + 2]) for i in range(0, 768, 3)]


def quantize(img_rgb, pal: list[tuple[int, int, int]], allow_fb: bool = False) -> bytes:
    import numpy as np
    from PIL import Image

    if not isinstance(img_rgb, Image.Image):
        img = Image.fromarray(img_rgb, "RGB")
    else:
        img = img_rgb.convert("RGB")
    arr = np.asarray(img, dtype=np.int16)
    h, w, _ = arr.shape
    pal_a = np.array(pal, dtype=np.int16)
    if not allow_fb:
        # avoid fullbright 224-254
        use = list(range(0, 224))
    else:
        use = list(range(256))
    pals = pal_a[use]
    # nearest
    flat = arr.reshape(-1, 3)
    # chunked for memory
    out = np.empty(flat.shape[0], dtype=np.uint8)
    step = 4096
    for i in range(0, flat.shape[0], step):
        chunk = flat[i : i + step]
        # distances to palette
        d = ((chunk[:, None, :] - pals[None, :, :]) ** 2).sum(axis=2)
        out[i : i + step] = np.array(use, dtype=np.uint8)[d.argmin(axis=1)]
    return out.tobytes()


def upscale_png(src: Path, dst: Path, scale: int, backend: str) -> None:
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


def remaster_mdl(
    src_mdl: Path,
    dst_mdl: Path,
    pal: list[tuple[int, int, int]],
    scale: int,
    backend: str,
    work: Path,
) -> None:
    from PIL import Image

    data = bytearray(src_mdl.read_bytes())
    if data[:4] != IDPO:
        raise ValueError(f"{src_mdl}: not IDPO")
    ver = struct.unpack_from("<i", data, 4)[0]
    if ver != ALIAS_VERSION:
        raise ValueError(f"{src_mdl}: version {ver}")

    numskins, skinwidth, skinheight = struct.unpack_from("<iii", data, 48)
    numverts, numtris, numframes = struct.unpack_from("<iii", data, 60)

    if skinwidth & 3:
        raise ValueError(f"{src_mdl}: skinwidth {skinwidth} not mult of 4")

    new_w = (skinwidth * scale) & ~3  # multiple of 4
    new_h = skinheight * scale
    if new_h > 2048:
        raise ValueError(f"{src_mdl}: skinheight {new_h} > 2048")

    pos = MDL_HDR_SIZE
    new_skins = bytearray()
    for si in range(numskins):
        skintype = struct.unpack_from("<i", data, pos)[0]
        pos += 4
        if skintype != ALIAS_SKIN_SINGLE:
            # group skins: skip for now (rare)
            raise ValueError(f"{src_mdl}: skin group not supported")
        skin = bytes(data[pos : pos + skinwidth * skinheight])
        pos += skinwidth * skinheight

        # to PNG
        img = Image.new("RGB", (skinwidth, skinheight))
        px = img.load()
        for y in range(skinheight):
            for x in range(skinwidth):
                px[x, y] = pal[skin[y * skinwidth + x]]
        stem = src_mdl.stem + f"_skin{si}"
        sp = work / f"{stem}.png"
        dp = work / f"{stem}_x{scale}.png"
        img.save(sp)
        upscale_png(sp, dp, scale, backend)
        up = Image.open(dp).convert("RGB")
        if up.size != (new_w, new_h):
            up = up.resize((new_w, new_h), Image.Resampling.NEAREST)
        # no fullbright — washes white in software
        indices = quantize(up, pal, allow_fb=False)
        new_skins += struct.pack("<i", ALIAS_SKIN_SINGLE)
        new_skins += indices

    # stverts
    st_off = pos
    st_size = numverts * 12  # onseam, s, t
    stverts = bytearray(data[st_off : st_off + st_size])
    for i in range(numverts):
        onseam, s, t = struct.unpack_from("<iii", stverts, i * 12)
        s2 = int(s * new_w / skinwidth)
        t2 = int(t * new_h / skinheight)
        struct.pack_into("<iii", stverts, i * 12, onseam, s2, t2)
    pos = st_off + st_size

    # rest of file (tris + frames) unchanged
    rest = bytes(data[pos:])

    # new header
    hdr = bytearray(data[:MDL_HDR_SIZE])
    struct.pack_into("<iii", hdr, 48, numskins, new_w, new_h)

    dst_mdl.parent.mkdir(parents=True, exist_ok=True)
    dst_mdl.write_bytes(bytes(hdr) + bytes(new_skins) + bytes(stverts) + rest)
    print(f"mdl {src_mdl.name}: {skinwidth}x{skinheight} -> {new_w}x{new_h} ({backend}×{scale})")


def cmd_build(args: argparse.Namespace) -> None:
    root = Path(args.quake_root).resolve()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    qpak = Path(args.qpak)
    game = root / args.game
    progs = game / "progs"
    progs.mkdir(parents=True, exist_ok=True)

    # palette
    subprocess.check_call(
        [str(qpak), "extract", str(root / args.pak), str(work / "ex"), "--exact", "gfx/palette.lmp"]
    )
    pal = load_palette(work / "ex/gfx/palette.lmp")

    # list mdls from pak
    data = (root / args.pak).read_bytes()
    diroff, dirsize = struct.unpack_from("<II", data, 4)
    mdls: list[str] = []
    for i in range(dirsize // 64):
        ent = data[diroff + i * 64 : diroff + i * 64 + 64]
        name = ent[:56].split(b"\0")[0].decode("latin1")
        if name.startswith("progs/") and name.endswith(".mdl"):
            mdls.append(name)

    # also pak1
    pak1 = root / "id1/pak1.pak"
    if pak1.is_file():
        data1 = pak1.read_bytes()
        diroff, dirsize = struct.unpack_from("<II", data1, 4)
        for i in range(dirsize // 64):
            ent = data1[diroff + i * 64 : diroff + i * 64 + 64]
            name = ent[:56].split(b"\0")[0].decode("latin1")
            if name.startswith("progs/") and name.endswith(".mdl") and name not in mdls:
                mdls.append(name)

    ok = 0
    fail = 0
    skin_work = work / "skins"
    skin_work.mkdir(parents=True, exist_ok=True)
    import shutil

    for name in sorted(mdls):
        # extract from pak0 then pak1
        src_path = work / "ex" / name
        src_path.parent.mkdir(parents=True, exist_ok=True)
        extracted = False
        for pak in (root / args.pak, pak1):
            if not pak.is_file():
                continue
            r = subprocess.run(
                [str(qpak), "extract", str(pak), str(work / "ex"), "--exact", name],
                capture_output=True,
            )
            if r.returncode == 0 and src_path.is_file():
                extracted = True
                break
        if not extracted:
            print(f"skip missing {name}")
            fail += 1
            continue
        try:
            remaster_mdl(
                src_path,
                progs / Path(name).name,
                pal,
                args.scale,
                args.backend,
                skin_work,
            )
            ok += 1
        except Exception as e:
            print(f"FAIL {name}: {e}")
            fail += 1
            shutil.copy(src_path, progs / Path(name).name)

    print(f"done models: {ok} ok, {fail} fail -> {progs}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("build")
    p.add_argument("--quake-root", default=".")
    p.add_argument("--pak", default="id1/pak0.pak")
    p.add_argument("--qpak", default="/mnt/qpaktool/build/release/qpak")
    p.add_argument("--work", default="/tmp/quake-hires-mdl")
    p.add_argument("--backend", choices=("xbrz", "esrgan"), default="xbrz")
    p.add_argument("--scale", type=int, default=2, help="2 recommended; 4 needs tall skins")
    p.add_argument("--game", required=True, help="e.g. hires_xbrz")
    args = ap.parse_args()
    if args.cmd == "build":
        cmd_build(args)


if __name__ == "__main__":
    main()
