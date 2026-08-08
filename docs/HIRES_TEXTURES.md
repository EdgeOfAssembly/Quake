# Hi-res textures for software quake.x11

## Run

```bash
./quake.x11 -basedir . -game hires -mem 256 -width 1920 -height 1080 -window +map e1m1
```

Loose files under `hires/textures/*.mip` override BSP-embedded textures
(`*` in names → `#` in filenames). Sky is never overridden (engine layout).

## Build pack (e1m1 x2)

```bash
source /mnt/python/bin/activate   # Pillow + numpy
python3 tools/hires_textures.py build-e1m1 --scale 2
```

Uses `qpak` + `realesrgan-ncnn-vulkan` + palette quantize.

## Memory

Default heap is 128 MB (`-mem` overrides). Surface cache still scales with
**framebuffer** resolution more than texture size.

## FPS (demo1, -nosound)

| Mode | 640×480 | 1920×1080 |
|------|---------|-----------|
| stock | ~585 | ~131 |
| hires x2 e1m1 | ~591 | ~130 |

Software remains palette-based; upscale helps detail but is not GL filtering.

## Palette quantize (important)

Naive nearest-RGB mapping put texels into **fullbright** indices (224–254) →
washed-out white walls.

Current pack uses:
- **No fullbright** unless the original texture used them
- Prefer colors from the **original** miptex (+ a few neighbors)
- **Floyd–Steinberg** dither + perceptual weights
- Max index in e1m1 pack: **222**

Rebuild:
```bash
python3 tools/hires_textures.py pack /tmp/quake-hires-work/png_x2 hires/textures \
  --palette hires/gfx/palette.lmp --original /tmp/quake-hires-work/png
```
