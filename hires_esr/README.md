# hires_esr

## Run
```bash
./quake.x11 -basedir . -game hires_esr -mem 512 -width 1280 -height 720 -window +map e1m1
```

## Contents
- `textures/` — world ×4 (xBRZ or ESRGAN)
- `gfx/palette.lmp`

## Not included yet (tools exist, quality WIP)
- `progs/*.mdl` — `tools/hires_models.py` (skin remap needs more work)
- `gfx.wad` — `tools/hires_gfx.py` (use stock + GuiScale HUD for now)

Rebuild textures: `python3 tools/hires_textures.py build-dual --scale 4`
