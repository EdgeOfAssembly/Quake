# Quake light sources catalogue

How lights actually appear in retail Quake, and how we should drive **true dynamic lighting**.

## Two different systems (do not confuse)

| Kind | What it is | e1m1? | Client sees model? |
|------|------------|-------|--------------------|
| **`light` entity** | Invisible point light; only bakes into **lightmaps** | **190** on e1m1 | No |
| **`light_torch_*` / `light_flame_*`** | QC entity → `progs/flame.mdl` / `flame2.mdl` | **0** on e1m1 | Yes (static) |
| **Wall light *textures*** | Brush faces textured `tlight*` etc. | **Many faces** | Geometry only |

**e1m1 (Slipgate Complex)** is almost all **wall light textures + invisible `light` entities**.  
Torches (`flame.mdl`) show up on **start**, **e1m2**, etc., not e1m1.

## e1m1 wall light textures (looked at)

Extracted from BSP miptex → `extracted/lights/e1m1_textures/*.png`  
Face positions → `extracted/lights/e1m1_wall_lights.txt`

| Texture | Size | Faces | Look / color (bright avg) | Role |
|---------|------|------:|---------------------------|------|
| **tlight01** | 32×32 | 16 | Small warm disc / lamp | Ceiling/wall puck |
| **tlight02** | 64×64 | 21 | **4 yellow panel lamps** on metal | Classic tech wall strip |
| **tlight07** | 32×128 | 8 | Tall **orange vertical tube** | Wall sconce / tube |
| **tlight08** | 32×128 | 45 | Dim brown panel (weak emitter) | Fixture housing |
| **tlight10** | 64×64 | 4 | **4 yellow lamps** (like tlight02) | Wall strip |
| **tlight11** | 16×64 | 67 | Horizontal **warm bar** | Thin light bar |
| **sliplite** | 16×16 | 12 | Bright **red** (slipgate) | Special / not room light |

**Takeaway:** emitters are **named `tlight*` textures** with warm yellow/orange pixels.  
Dynamic lights should be placed at **face centroids** of those surfaces, with RGB sampled from the texture (or fixed warm defaults).

## Entity lights (other maps / QC)

From `QW/progs/misc.qc` + full campaign BSP scrape:

| Classname | Model | Count (all e* maps) | Suggested RGB |
|-----------|--------|---------------------:|---------------|
| `light` | (none) | 6747 | warm white / `_color` if set |
| `light_torch_small_walltorch` | `progs/flame.mdl` | 392 | (1.0, 0.55, 0.15) |
| `light_flame_large_yellow` | `progs/flame2.mdl` | 168 | (1.0, 0.65, 0.2) |
| `light_flame_small_yellow` | `progs/flame2.mdl` | 102 | (1.0, 0.6, 0.18) |
| `light_flame_small_white` | `progs/flame2.mdl` | 13 | (1.0, 0.95, 0.85) |
| `light_fluoro` / `fluorospark` | (none + hum) | 46 | (0.7, 0.9, 1.0) |
| `light_globe` | `progs/s_light.spr` | 2 | (1.0, 0.95, 0.8) |

Models extracted: `extracted/lights/progs/flame.mdl`, `flame2.mdl`, `s_light.spr`, …

## What the engine does today

- **Baked lightmaps** from `light` entities (static, gray).
- **Dlights** (`dlight_t`): radius only, **no RGB** — muzzle flash, rockets, explosions.
- **GL flashblend**: orange-ish additive sphere (`R_RenderDlight`), not tied to torches.
- **Hack:** `flame.mdl` / `flame2.mdl` drawn fullbright (self-lit mesh only).

So wall `tlight*` panels look “lit” only because of **precomputed lightmaps**, not because the engine knows they are lamps.

## Target design: auto dynamic lights (hardware)

**GPU:** GTX 1050 4GB — use **OpenGL** path already in `glquake` (CUDA unnecessary for ≤ few hundred point lights; Vulkan = full rewrite).

### Detection (automatic)

1. **On map load** (parse `cl.worldmodel->entities` + BSP faces):
   - Every face whose texture name matches `tlight*` (and optional allow-list) → light probe at face center, normal offset a few units into the room, RGB from texture bright-average or table.
   - Every entity `light`, `light_*`, `light_torch_*`, `light_flame_*` → probe at `origin`, radius from `light` key (default 200–300).
2. **Each frame** (models):
   - Any static/dynamic entity with model `progs/flame.mdl` / `flame2.mdl` / `s_light.spr` → update a dlight (flicker optional).

### Rendering (GL, accelerated)

1. Extend `dlight_t` with `float color[3]`.
2. **Surfaces:** colored contribution in `R_AddDynamicLights` / lightmap upload (RGB lightmap or additive pass) — real “lights the floor.”
3. **Flashblend / mesh:** `R_RenderDlight` uses `light->color` (cheap, already GL blend).
4. Optional cvars:
   - `r_dynlights 1` — enable
   - `r_dynlights_map 1` — texture + entity probes
   - `r_dynlights_bake_scale 0.3` — dim baked lightmaps so dynamic lights dominate indoors
   - `r_dynlights_max 64` — raise `MAX_DLIGHTS` as needed

### Not required for v1

- CUDA light baking (offline tool later).
- Vulkan renderer.
- ML “detect torch in screenshot” — **BSP texture names already label them.**

## Commands used

```bash
qpak list id1/pak0.pak --wildcard 'progs/flame*'
qpak extract id1/pak0.pak extracted/lights --wildcard 'progs/flame*'
# BSP texture dump + face centroids: see scripts below / one-shot in session
```

## Files

| Path | Content |
|------|---------|
| `extracted/lights/e1m1_textures/*.png` | Looked-at wall light textures |
| `extracted/lights/e1m1_wall_lights.txt` | Face origins per texture |
| `extracted/lights/progs/*` | Flame / light models |
| `docs/LIGHT_SOURCES.md` | This catalogue |

## e1m1 ceilings and sky (BSP analysis)

### Sky (not a modern skybox)

| | |
|--|--|
| Texture | **`sky4`** (256×128 scrolling sky) |
| Faces | **63** (55 ceiling-facing, 8 wall) |
| worldspawn | no `sky` key; `worldtype 2` only |

Open sky is **brush faces with `sky4`**, classic Quake sky — not a cubemap skybox.

### Ceiling composition

| Orientation | Face count |
|-------------|----------:|
| Wall | 3327 |
| Ceiling | 1185 |
| Floor | 1004 |

Most ceilings are **tech metal** (`tech04_1`, `tech04_3`, `tech01_*`, …), not lamps. **55** ceiling faces are **`sky4`** (outdoors).

### Light textures by orientation (e1m1)

| Texture | Ceiling | Wall | Floor | Notes |
|---------|--------:|-----:|------:|-------|
| tlight01 | **11** | 4 | 1 | Mostly **ceiling pucks** |
| tlight02 | **8** | 13 | 0 | Ceiling strips + walls |
| tlight10 | **3** | 1 | 0 | Mostly ceiling |
| tlight08 | 8 | 20 | 17 | Mixed fixture body |
| tlight11 | 4 | **60** | 3 | Almost all **walls** |
| tlight07 | 0 | **8** | 0 | **Wall tubes only** |
| sliplite | 4 | 0 | 8 | Slipgate red |

**Dynamic light placement:** ceiling `tlight*` → offset along −normal (into room); wall → offset along outward normal into room; skip pure `sky4` for yellow lamps (use ambient/outdoor instead).
