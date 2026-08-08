# hires_xbrz — Quake hi-res pack (xbrz ×4)

## Run

```bash
./quake.x11 -basedir . -game hires_xbrz -mem 512 -width 1280 -height 720 -window +map e1m1
```

## Backend

- **xbrz** — pixel-art scaler (sharp, on-palette, safe for water)
- **esrgan** — Real-ESRGAN neural (softer, more detail; color can drift)

Stack packs (later wins):

```bash
./quake.x11 -basedir . -game hires_esr -game hires_xbrz ...
```

Rebuild: `python3 tools/hires_textures.py build-dual --scale 4`

## Contents
- `textures/` — world ×4
- `progs/` — model skins ×2
- HUD uses **stock gfx.wad** + runtime GuiScale (pre-scaled gfx.wad optional via tools/hires_gfx.py)
