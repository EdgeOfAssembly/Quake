# hires_esr — Quake hi-res pack (esrgan ×4)

## Run

```bash
./quake.x11 -basedir . -game hires_esr -mem 512 -width 1280 -height 720 -window +map e1m1
```

## Backend

- **xbrz** — pixel-art scaler (sharp, on-palette, safe for water)
- **esrgan** — Real-ESRGAN neural (softer, more detail; color can drift)

Stack packs (later wins):

```bash
./quake.x11 -basedir . -game hires_esr -game hires_xbrz ...
```

Rebuild: `python3 tools/hires_textures.py build-dual --scale 4`
