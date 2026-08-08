# Hi-res texture packs for software quake.x11

## Two packs (pick one)

| Gamedir | Backend | Feel |
|---------|---------|------|
| **`hires_xbrz`** | [xBRZ](https://sourceforge.net/projects/xbrz/) ×4 | Sharp, on-palette, pixel-art faithful |
| **`hires_esr`** | Real-ESRGAN ×4 | Softer, more “remaster” detail |

```bash
# Pixel-art style (recommended default for stock Quake look)
./quake.x11 -basedir . -game hires_xbrz -mem 512 -width 1280 -height 720 -window +map e1m1

# Neural remaster style
./quake.x11 -basedir . -game hires_esr -mem 512 -width 1280 -height 720 -window +map e1m1
```

**Override order:** later `-game` wins for the same path:

```bash
./quake.x11 -basedir . -game hires_esr -game hires_xbrz ...
# → xBRZ textures override ESR where both exist
```

Legacy **`hires/`** may still exist from earlier work; prefer **`hires_xbrz`** / **`hires_esr`**.

## Rebuild e1m1 dual packs

```bash
source /mnt/python/bin/activate   # Pillow, etc.
# needs: xbrzscale, realesrgan-ncnn-vulkan, qpaktool
python3 tools/hires_textures.py build-dual --scale 4
# optional better ESRGAN:
python3 tools/hires_textures.py build-dual --scale 4 --tta
```

Single pack:

```bash
python3 tools/hires_textures.py build-e1m1 --backend xbrz --game hires_xbrz --scale 4
python3 tools/hires_textures.py build-e1m1 --backend esrgan --game hires_esr --scale 4
```

Manual steps:

```bash
python3 tools/hires_textures.py extract-bsp id1/maps/e1m1.bsp -o /tmp/tex-src --palette id1/gfx/palette.lmp
python3 tools/hires_textures.py upscale /tmp/tex-src /tmp/tex-xbrz -s 4 --backend xbrz
python3 tools/hires_textures.py pack /tmp/tex-xbrz hires_xbrz/textures \
  --palette id1/gfx/palette.lmp --original /tmp/tex-src
```

## Policy (source art)

1. **Keep images as large as possible** (×4).
2. **Do not downscale in the pipeline** for “engine convenience.”
3. **Engine** uses size as-is or **box-downscales** to multiple of **16**, edge **≤ 1024**.

## External formats (per texture name, `*` → `#`)

1. `textures/<name>.tga` — 24/32-bit (preferred truecolor)
2. `textures/<name>.rgba` — raw RGBA
3. `textures/<name>.mip` — classic 8-bit

## UV scale + full-res surface cache

| Field | Meaning |
|-------|---------|
| `base_width` / `base_height` | BSP UV space (original size) |
| `width` / `height` | Actual mip / RGBA pixel size |

Lightmaps stay in base UV; surface cache is hires-scaled. Prefer `-mem 256` or `512`.

## HUD / fonts

UI scale is **resolution-based** (`Draw_GuiScale` = height/240), **not** texture ×4.
Menus and status bar scale together; world art is independent.

## Notes

- **32bpp + RGBA:** lit world samples `texture_t.rgba`; turb uses truecolor warp in base UV space.
- **Sky** not overridden.
- **xBRZ** is usually safer for water/flat tiles; **ESRGAN** can wash colors (use color-constrained pack).
- Future: gfx.wad / model skins can live in the same gamedirs with the same `-game` switch.

## Models + HUD (gfx.wad)

```bash
# skins ×2 (fits engine; ×4 needs MAX_LBM_HEIGHT 2048 — already raised)
python3 tools/hires_models.py build --backend xbrz --scale 2 --game hires_xbrz
python3 tools/hires_models.py build --backend esrgan --scale 2 --game hires_esr

# HUD / menu pics ×4 into gfx.wad
python3 tools/hires_gfx.py build --backend xbrz --scale 4 --game hires_xbrz
python3 tools/hires_gfx.py build --backend esrgan --scale 4 --game hires_esr
```

Packs include `progs/*.mdl` and `gfx.wad`. HUD draw uses **PicFit** so pre-×4
lumps still match `Draw_GuiScale` layout.
