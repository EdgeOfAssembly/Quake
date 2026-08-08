# Hi-res textures for software quake.x11

## Policy (source art)

1. **Keep images as large as possible** (e.g. Real-ESRGAN **×4 + TTA**).
2. **Do not downscale in the pipeline** for “engine convenience.”
3. **Engine** either uses the size as-is (if valid) or **box-downscales** to the
   next supported size: **multiple of 16**, edge **≤ 1024** (`Image_FitTextureSize`).

## Run

```bash
./quake.x11 -basedir . -game hires -mem 512 -width 1920 -height 1080 -window +map e1m1
```

## External formats (per texture name, `*` → `#`)

1. `textures/<name>.tga` — 24/32-bit (preferred truecolor)
2. `textures/<name>.rgba` — raw RGBA
3. `textures/<name>.mip` — classic 8-bit

### Transparency → alpha

- Image alpha &lt; 128 → transparent  
- RGB = **palette index 255** → transparent (Quake key)  
- Magenta (255,0,255) → transparent  
- Software mips: transparent → **index 255**

## Rebuild e1m1 ×4 + TTA

```bash
source /mnt/python/bin/activate
# extract + realesrgan -s 4 -x, then pack (no downscale)
python3 tools/hires_textures.py build-e1m1 --scale 4 --tta
# or manual: upscale file-by-file with -x, then:
python3 tools/hires_textures.py pack /tmp/.../png_x4tta hires/textures \
  --palette hires/gfx/palette.lmp --original /tmp/.../png_src
# export TGA for RGBA path from those PNGs
```

## UV scale + full-res surface cache

BSP `texturemins` / extents are authored for the **original** miptex size.
Hires replacements may be larger (`width`/`height`); the engine stores:

| Field | Meaning |
|-------|---------|
| `base_width` / `base_height` | BSP UV space (original size) |
| `width` / `height` | Actual mip / RGBA pixel size |

| Stage | Space |
|-------|--------|
| Lightmaps | **Base** UV (unchanged BSP extents) |
| Surface cache | **Hires** — `surfwidth = (extents>>mip) * (width/base_width)` |
| Span UVs | Scaled in `D_CalcGradients` to match cache |
| Block fill | Sample source **1:1** into the larger cache |

So bolts keep correct world scale **and** show full hires detail. Pool size is
×8 vs stock (`D_SurfaceCacheForRes`); override with `-surfcachesize <KB>`.
Prefer `-mem 256` or `512` with hires + 32bpp.

## Notes

- Software still **draws** 8-bit + colormap into the surface cache; RGBA is kept for quality conversion / future truecolor light.
- Sky not overridden.
- ×4 pack is larger (~30MB+); use `-mem 256` or `512`.

## Native 32-bit software draw

On X11 TrueColor (depth 24, bpp 32), the engine sets **`r_pixbytes = 4`**:

- World spans write **native 32-bit** pixels (`D_DrawSpans32bpp`)
- Surface cache stores 4 bytes/texel (`R_DrawSurfaceBlock32`)
- **No** `st3_fixup` 8→32 expand at present
- Particles, sprites, alias, HUD expand 8-bit art via `d_8to24table`
- Log line: `VID: r_pixbytes=4 (depth=24 bpp=32)`

8-bit PseudoColor still uses `r_pixbytes=1` + fixup.
