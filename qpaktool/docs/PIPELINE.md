# Quake asset pipeline (PAK → Blender / Xmux)

How we extract classic Quake1 monsters (ogre, shambler, …), convert skins, and
view them with GPU Blender inside an Xmux spectator session.

## Where assets live

| Path | Content |
|------|---------|
| `id1/PAK0.PAK` | Base monsters: **ogre**, **shambler**, grunt, dog, … + `gfx/palette.lmp` |
| `id1/PAK1.PAK` | Registered extras (enforcer, hell knight, …) — **no** ogre/shambler |

Example host tree: `/tmp/QUAKE/id1/PAK0.PAK` (GOG layout).

## Texture fact

Monster “textures” are **not** separate TGA/PNG files in the PAK.

- Skin is **embedded** in `progs/<name>.mdl` (8-bit indices)
- Colors come from **`gfx/palette.lmp`** (256×RGB)
- Convert with `scripts/mdl_to_obj.py` → `*_skin.png` + `.obj` + `.mtl`

## Tool: `qpak` (this directory)

C++23 CLI, g++ only. Spec: [quakewiki .pak](https://quakewiki.org/wiki/.pak).

```bash
cd quake/qpak
make -s release
make -s test
# optional: install -m 755 build/release/qpak ~/.local/bin/qpak

qpak                          # usage
qpak list <pak>
qpak list <pak> --exact progs/shambler.mdl
qpak list <pak> --wildcard 'progs/*.mdl'   # default if pattern given
qpak list <pak> --regex 'ogre|shambler'
qpak extract <pak> <outdir>
qpak extract <pak> <outdir> --wildcard '*sham*'
```

## Convert MDL → OBJ + PNG

```bash
qpak extract /path/to/id1/PAK0.PAK /tmp/quake-extract --wildcard '*sham*'
qpak extract /path/to/id1/PAK0.PAK /tmp/quake-extract --exact gfx/palette.lmp

python3 scripts/mdl_to_obj.py \
  /tmp/quake-extract/progs/shambler.mdl \
  /tmp/quake-extract/gfx/palette.lmp \
  -o /tmp/quake-shambler-blender
# → shambler.obj, shambler_skin.png, shambler.mtl
```

## View in Xmux + Blender (NVIDIA PRIME)

Requires Xmux with `--gl nvidia` (client PRIME; X server stays 2D).

```bash
# session
xmux start quake-shambler --geometry 1280x800 --gl nvidia
# human spectator (no typing race):
#   /path/to/Xmux/scripts/auto-attach.sh quake-shambler

xmux run quake-shambler -- env __GL_SYNC_TO_VBLANK=0 \
  QUAKE_OBJ=/tmp/quake-shambler-blender/shambler.obj \
  QUAKE_SKIN=/tmp/quake-shambler-blender/shambler_skin.png \
  QUAKE_NAME=Shambler \
  TURN_PERIOD_S=18 \
  blender --factory-startup --python scripts/blender_quake_monster.py
```

`scripts/blender_quake_monster.py`:

- Loads OBJ + nearest-neighbor skin
- **Orientation:** prefer Quake **Z-up** if Z ≥ 85% of max bbox axis  
  (arms-out `stand1` can make Y slightly larger — do **not** blindly rotate)
- **Camera:** elevated ~18°, aim ~55% height (chest) — avoids default **crotch-cam**
- Continuous **360°** yaw, default 18 s/rev

### Why default views look like crotch shots

1. Bbox center ≈ pelvis on bipeds  
2. Flat default elevation looks *up* into midsection  
3. “Tallest axis” auto-stand can tip models on their side (shambler)

Always **screenshot** after import (`xmux screenshot <session> -o /tmp/check.png`) and look at the image before calling it done.

## Proven demos (2026-07)

| Monster | PAK path | Verts / tris | Notes |
|---------|----------|--------------|--------|
| Ogre | `progs/ogre.mdl` | 169 / 326 | Chainsaw; Z already tallest |
| Shambler | `progs/shambler.mdl` | 144 / 284 | Lightning; needed Z-up preference |

## Skin upscale (Real-ESRGAN)

Host tool: `realesrgan-ncnn-vulkan` (Vulkan; GTX 1050 works).

```bash
# Models: /usr/share/realesrgan-ncnn-vulkan/models
realesrgan-ncnn-vulkan \
  -i shambler_skin.png \
  -o shambler_skin_x4.png \
  -s 4 \
  -n realesrgan-x4plus \
  -m /usr/share/realesrgan-ncnn-vulkan/models \
  -f png -v
```

| Model (`-n`) | Notes on Quake skins |
|--------------|----------------------|
| **realesrgan-x4plus** | Prefer for organic/bloody monsters (shambler winner) |
| realesrgan-x4plus-anime | Cleaner/flatter; less grimy detail |

**308×115 → 1232×460** at `-s 4`. Same UVs; only swap the image in Blender.

**Honest result:** upscale alone is subtle on a spinning low-poly mesh. **Poly density** is what reads as “scary remaster.”

## Mesh densify (subdiv + maw)

Script: `scripts/blender_shambler_hipoly.py` (generalizable pattern).

| Stage | Verts | Polys |
|-------|------:|------:|
| Stock MDL (shambler) | 144 | 284 |
| Catmull–Clark subdiv **L2** (applied) | ~3410 | ~3408 |
| + head/maw `bmesh` edge subdiv + smooth | **~6569** | **~6651** |

```bash
# after extract + mdl_to_obj + realesrgan x4:
xmux start quake-shambler --geometry 1280x800 --gl nvidia
xmux run quake-shambler -- env __GL_SYNC_TO_VBLANK=0 \
  QUAKE_OBJ=/tmp/quake-shambler-blender/shambler.obj \
  QUAKE_SKIN=/tmp/quake-shambler-blender/shambler_skin_x4.png \
  blender --factory-startup --python scripts/blender_shambler_hipoly.py
# human: scripts/auto-attach.sh quake-shambler  (from Xmux tree)
```

**Always** capture before/after with `xmux screenshot` and **read the images** (skin-only changes are easy to overclaim).

Example evidence paths from the session:

- `/tmp/shambler-BEFORE-1.png` — 144v faceted  
- `/tmp/shambler-AFTER-1.png` — hipoly smooth  
- `/tmp/shambler-BEFORE-AFTER.png` — contact sheet  

Further “nightmare” work (open jaw, teeth, gums) needs **sculpt**, not more blind subdiv.

## Specs / links

- https://quakewiki.org/wiki/.pak  
- Unofficial Quake Specs §5 (Alias / IDPO models)  
- Host note file may list: `/tmp/QUAKE.txt`
