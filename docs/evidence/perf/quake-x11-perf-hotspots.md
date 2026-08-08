# quake.x11 software-renderer performance hotspots

**Scope:** `/home/wizard/Quake/WinQuake` software path only (`quake.x11` / WinQuake SW).  
**Not in scope:** `glquake`, GL dynlights, GPU paths.  
**Date:** 2026-08-08  
**Method:** static code review of focus files + build config. No timedemo/`perf` run in this pass (claims about ranking are classic-Quake + structure-based; verify with `r_dspeeds` / `perf record`).

---

## Build context (why C paths matter)

| Fact | Evidence |
|------|----------|
| x86_64 builds pure C | `Makefile.linux`: “pure C — id386=0 on x86_64”; no `.s` objects in `X11_SRCS` |
| `id386` only on 32-bit i386 | `quakedef.h`: `#if defined __i386__` → `id386 1`, else `0` |
| All classic ASM kernels inactive | `d_scan.c`, `r_edge.c`, `r_surf.c`, `d_polyse.c`, `d_part.c`, `d_sprite.c`, `r_draw.c`, `r_aclip.c` use `#if !id386` C bodies |
| Span drawer always 8-pixel C | `d_init.c`: non-id386 forces `d_drawspans = D_DrawSpans8` (no `D_DrawSpans16` C port) |
| Required aliasing flag | `Makefile.linux` `OPTFLAGS`: `-fno-strict-aliasing` (type-punning in Z-span packing, solid fill, etc.) |
| Release opts already aggressive | `-O3 -ffast-math -funroll-loops -fomit-frame-pointer` + `$(MARCH)` |

**Implication:** every classic “ASM was 2–5× faster” hotspot is now a pure-C inner loop. That is the main modern-CPU opportunity surface.

---

## 1. Classic Quake soft-renderer bottlenecks

Frame pipeline (`r_main.c` → `R_RenderView_`):

```
R_MarkLeaves
  → R_EdgeDrawing
       R_RenderWorld          (BSP walk + emit edges)
       R_DrawBEntitiesOnList  (brush models → more edges)
       R_ScanEdges            (AET scan → spans → D_DrawSurfaces)
  → R_DrawEntitiesOnList      (alias / sprites)
  → R_DrawViewModel
  → R_DrawParticles
  → D_WarpScreen (underwater)
  → VID_Update (X11 blit; outside SW raster but often top of wall-clock)
```

Built-in timers: `r_speeds` / `r_dspeeds` (`R_PrintTimes`, `R_PrintDSpeeds` in `r_misc.c`):

| Tag | Meaning |
|-----|---------|
| `w` / `rw_time` | world BSP + face emit |
| `b` / `db_time` | brush entities |
| `s` / `se_time` | scan edges + surface draw (often largest) |
| `e` / `de_time` | alias/sprite entities |
| `v` / `dv_time` | viewmodel |
| `p` / `dp_time` | particles |

### 1.1 Span drawing (usually #1 pixel cost)

| Function | File | Role |
|----------|------|------|
| **`D_DrawSpans8`** | `d_scan.c:257` | Opaque world/bmodel textured spans |
| **`D_DrawZSpans`** | `d_scan.c:395` | Z-buffer write for every drawn span |
| **`Turbulent8` / `D_DrawTurbulent8Span`** | `d_scan.c:122,100` | Water/slime warp |
| **`D_DrawSkyScans8`** | `d_sky.c:65` | Sky spans |
| **`D_SpriteDrawSpans`** | `d_sprite.c:37` | Sprites (tex + Z test + color-key 255) |
| **`D_PolysetDrawSpans8`** | `d_polyse.c:614` | Alias model affine spans |

**Why expensive:**

- Perspective-correct texture: float `s/z`, `t/z`, `1/z` setup, then **`z = 0x10000 / zi` every 8 pixels** (`D_DrawSpans8`), every 16 for turb.
- Inner pixel loop is scalar, dependent, and memory-random:

```c
// d_scan.c ~370–375
*pdest++ = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
s += sstep;
t += tstep;
```

- `cachewidth` multiply every pixel; texture reads thrash L1/L2.
- Z path packs two shorts via intentional type-pun (`*(int *)pdest`) — needs `-fno-strict-aliasing`.
- Driver dispatch: `(*d_drawspans)(s->spans)` from `D_DrawSurfaces` (`d_edge.c:308`).

### 1.2 Edge raster / span generation (usually #1–2 structure cost)

| Function | File | Role |
|----------|------|------|
| **`R_ScanEdges`** | `r_edge.c:663` | Per-scanline AET driver |
| **`R_InsertNewEdges`** | `r_edge.c:173` | Sorted insert into AET |
| **`R_StepActiveU`** | `r_edge.c:234` | Advance U + re-sort |
| **`R_GenerateSpans` / `R_LeadingEdge` / `R_TrailingEdge`** | `r_edge.c:588,460,416` | Emit `espan_t` for top surface |
| **`R_EmitEdge`** | `r_draw.c:80` | Project edge, build edge_t |
| **`R_RenderFace`** | `r_draw.c:386` | Clip face → edges |
| **`R_RecursiveWorldNode`** | `r_bsp.c:448` | PVS + frustum cull + face emit |

**Why expensive:**

- O(height × active_edges) with pointer-chasing doubly-linked lists.
- `R_InsertNewEdges` / `R_StepActiveU` use unrolled compare ladders + `goto` (ASM-era style; hard for modern branch predictors when edge density is high).
- `R_LeadingEdge` does float 1/z tests for coplanar bmodels.
- Span buffer can flush mid-frame (`span_p >= max_span_p` → early `D_DrawSurfaces`), adding cache churn.
- `R_BeginEdgeFrame` clears `newedges[]`/`removeedges[]` with a **per-scan loop** (`r_edge.c:153–157`) instead of `memset` (comment: `FIXME: set with memset`).

### 1.3 Surface lighting / lightmapped cache (cache-miss spikes)

| Function | File | Role |
|----------|------|------|
| **`D_CacheSurface`** | `d_surf.c:264` | Surfcache lookup / rebuild |
| **`R_DrawSurface`** | `r_surf.c:248` | Orchestrate lightmap + block draw |
| **`R_BuildLightMap`** | `r_surf.c:149` | Static styles + dlights → `blocklights[]` |
| **`R_AddDynamicLights`** | `r_surf.c:61` | Per-luxel dlight falloff |
| **`R_DrawSurfaceBlock8_mip0..3`** | `r_surf.c:343+` | 16×16…2×2 lightmap×texel → 8-bit via `vid.colormap` |

**Why expensive:**

- On cache miss / lightstyle change / dlight: full surface re-light.
- Block drawers are nested loops with **colormap indirection** every texel:

```c
// r_surf.c ~368–372 (mip0)
pix = psource[b];
prowdest[b] = ((unsigned char *)vid.colormap)[(light & 0xFF00) + pix];
light += lightstep;
```

- Globals (`lightleft`, `lightright`, `r_lightptr`, …) used across loops; comments already say `FIXME: make these locals?` / `use delta rather than both right and left, like ASM?`.
- `R_AddDynamicLights` is O(dlights × smax × tmax) with abs + approximate dist.
- `c_surf` (`r_speeds`) counts rebuilds — high values mean lighting, not just span fill, is dominating.

### 1.4 Alias / particles / misc

| Function | File | Notes |
|----------|------|-------|
| **`D_PolysetDrawSpans8`** | `d_polyse.c` | Per-pixel Z-test + skin + light colormap; demo1 monsters/weapons |
| **`D_PolysetCalcGradients`** | `d_polyse.c:531` | Float gradients + `ceil` for light steps |
| **`D_DrawParticle`** | `d_part.c:55` | Transform + project + small Z-tested square; many particles → measurable |
| **`R_MarkLights` / `R_PushDlights`** | `r_light.c` | BSP walk per dlight |
| **`RecursiveLightPoint`** | `r_light.c:141` | Alias lighting samples |
| **`D_WarpScreen`** | `d_scan.c:44` | Full-viewport gather when underwater |
| **`VID_Update` / XShm** | `vid_x.c` | Often top of *wall-clock* at high res; not SW math but real FPS |

### 1.5 Known structural waste already marked in tree

`d_edge.c` `D_DrawSurfaces` for `insubmodel`:

- Per-polygon `R_RotateBmodel()` + frustum restore (`FIXME: we don't want to do all this for every polygon!` / `TODO: store once at start of frame` / `TODO: speed up`).
- Same pattern for turb and opaque bmodel surfaces.

---

## 2. Obvious modern-CPU wins currently missing

**Constraint:** keep `-fno-strict-aliasing`. Do **not** “fix” type-pun by enabling strict aliasing. Prefer `memcpy`/`uint32_t` loads only where behavior is proven identical.

### 2.1 Completely absent today

| Technique | Status in WinQuake SW | Notes |
|-----------|----------------------|-------|
| `restrict` / `__restrict` | **None** (only unrelated “restrict” English in comments/headers) | Safe on non-aliased span dest vs texture base |
| `__builtin_expect` / `likely`/`unlikely` | **None** | Span empty checks, clip rejects, cache hit path |
| SIMD (SSE2/AVX2/NEON) | **None** in C path | Hard for perspective spans; easier for solid fill, Z dual-write, lightmap add, colormap if restructured |
| Software prefetch | **None** | Texture rows / next span dest |
| C port of `D_DrawSpans16` | **Missing** (ASM-only historically) | 16-pixel subdivision = half the FP divides vs 8 |
| Profile-guided opts | Not wired | Would help branchy edge code |

### 2.2 Low-hanging structural issues (compiler-hostile)

1. **Hot state in globals**  
   Turb: `r_turb_s`, `r_turb_pdest`, …  
   Surf blocks: `lightleft`, `pbasesource`, `prowdestbase`, …  
   Prevents register allocation / inlining clarity. Locals (as FIXMEs already suggest) help without behavior change.

2. **Indirect calls in hot paths**  
   `(*d_drawspans)`, `(*pblockdrawer)`, `(*pdrawfunc)` — block cross-module inlining unless LTO.

3. **Per-pixel `* cachewidth`**  
   Could step a row pointer or use `t>>16` with pre-shifted pitch when `cachewidth` is power-of-two (often is for mips).

4. **FP divide cadence**  
   Only 8-pixel steps in C; classic ASM used 16 (`d_subdiv16`). Porting 16-pixel C subdivision is a direct win.

5. **`R_BeginEdgeFrame` zeroing**  
   Loop vs `memset` for `newedges`/`removeedges`.

6. **Bmodel matrix thrash**  
   `R_RotateBmodel` per surface in `D_DrawSurfaces`.

7. **`CACHE_SIZE` still 32** (`quakedef.h`)  
   Harmless for correctness; 64 would match modern lines for stack span/edge alignment (minor).

8. **No `restrict` on span kernels**  
   e.g. `D_DrawSpans8(espan_t *pspan)` with local `byte *restrict pdest` and `const byte *restrict pbase` after proving no alias with globals used in that loop.

9. **Colormap lookup**  
   `((byte *)vid.colormap)[(light & 0xFF00) + pix]` — unavoidable for 8-bit Quake look; can still hoist colormap base to a local pointer.

10. **Already present (good)**  
    `-O3`, `-ffast-math`, `-funroll-loops`, manual 4× unroll in `D_WarpScreen` and solid fill, dual-short Z write, mip table of specialized block drawers.

### 2.3 Unsafe / out-of-scope “wins” (do not do casually)

- Enabling strict aliasing or rewriting all punning without tests.
- Changing fixed-point wrap semantics (debug builds use `-fwrapv` for a reason).
- Approximate lighting that changes demo1 pixels (unless optional cvar).
- Dropping Z writes / overdraw correctness.

---

## 3. Functions typically top of gprof/perf for `timedemo demo1`

**Caveat:** not measured in this explore pass. Ranking is the standard WinQuake SW profile shape for demo1 (start map run, mixed world + a few monsters + gun + particles), scaled up at modern resolutions (e.g. 800×600 / 1920×1080).

### Expected top tier (often >50% of raster time combined)

| Rank (typical) | Symbol | Why on demo1 |
|----------------|--------|--------------|
| 1 | **`D_DrawSpans8`** | Nearly every world pixel |
| 2 | **`D_DrawZSpans`** | Same span count as opaque/turb/sky |
| 3 | **`R_DrawSurfaceBlock8_mip0`** (and mip1) | Surfcache fills; mip0 when close |
| 4 | **`R_ScanEdges` + `R_GenerateSpans` + `R_LeadingEdge`** | Full-height edge walk |
| 5 | **`R_InsertNewEdges` / `R_StepActiveU`** | AET maintenance |
| 6 | **`D_PolysetDrawSpans8`** | Viewmodel + any monsters in view |
| 7 | **`R_BuildLightMap` / `R_DrawSurface` / `D_CacheSurface`** | Spikes when styles/dlights/cache churn |
| 8 | **`R_EmitEdge` / `R_RenderFace`** | World complexity |
| 9 | **`Turbulent8`** | If water in frustum (map-dependent) |
| 10 | **`VID_Update` / X11 SHM** | Display path; grows with resolution |

### Second tier

- `R_RecursiveWorldNode`, `R_MarkLeaves`
- `D_DrawParticle` / `R_DrawParticles`
- `D_SpriteDrawSpans` (explosions, etc.)
- `R_AddDynamicLights` when rocket/shotgun lights active
- `D_WarpScreen` only when submerged
- `R_Alias*` transform/clip (`r_alias.c`, `r_aclip.c`) — usually below span fill unless many models

### How to measure on this tree

```bash
# In-engine breakdown
./quake.x11 -basedir . -width 800 -height 600 +timedemo demo1
# then: r_dspeeds 1  /  r_speeds 1

# External
perf record -g -- ./quake.x11 -basedir . +timedemo demo1
perf report
# or: gprof after -pg rebuild
```

At **Full HD**, expect span + Z + X11 blit to grow roughly with pixel count; edge code grows with resolution×edge density; lightmap rebuild cost is more view- and dlight-dependent than pure resolution.

---

## 4. Safe optimization candidates (ranked by risk)

Risk = chance of pixel/demo desync, UB, or hard-to-bisect bugs.  
All assume **keep `-fno-strict-aliasing`** and validate with `timedemo demo1` + visual smoke.

### LOW risk (behavior-preserving / mechanical)

| ID | Candidate | Where | Expected gain | Notes |
|----|-----------|-------|---------------|-------|
| L1 | **`memset` for `newedges`/`removeedges`** | `r_edge.c` `R_BeginEdgeFrame` | Small but free | Already FIXME |
| L2 | **Locals instead of globals in surface block drawers** | `r_surf.c` mip0–3 | Small–medium | Matches existing FIXME; better reg alloc |
| L3 | **Hoist `vid.colormap` / `acolormap` to local `byte *`** | `r_surf.c`, `d_polyse.c` | Small | No semantic change |
| L4 | **`restrict` on local pointers in span loops** | `d_scan.c`, `d_sprite.c`, `d_polyse.c` | Small–medium | Only after dest/tex/z proven non-aliasing within loop |
| L5 | **`likely`/`unlikely` on cache hit & empty span** | `d_surf.c` `D_CacheSurface`, span count checks | Small | |
| L6 | **LTO (`-flto`) for release `quake.x11`** | `Makefile.linux` | Medium | Helps indirect `d_drawspans` / mip table; test carefully with `-ffast-math` |
| L7 | **Avoid per-pixel `* cachewidth` when pitch is const** | `D_DrawSpans8` | Small–medium | Use pointer step or shift if power-of-two |
| L8 | **Entity-level bmodel transform once** | `d_edge.c` `D_DrawSurfaces` | Medium on bmodel-heavy views | Group by `s->entity` or cache rotated basis; large FIXME already |
| L9 | **Specialized solid-span / sky already OK; ensure `-march=native` in real builds** | Makefile `MARCH` | Env-dependent | Confirm ship flags |

### MEDIUM risk (same look if careful; needs timedemo + screenshots)

| ID | Candidate | Where | Expected gain | Notes |
|----|-----------|-------|---------------|-------|
| M1 | **C `D_DrawSpans16` (16-pixel subdivision)** | `d_scan.c` + `d_init.c` | **High** on world fill | Restore `d_subdiv16` path; must match clamp/step edge cases of 8-pixel version |
| M2 | **C turb span 16-step already uses 16; tighten inner turb** | `D_DrawTurbulent8Span` | Medium in water | Unroll 2–4; keep CYCLE masks |
| M3 | **Faster `R_BuildLightMap` accumulation** | `r_surf.c` | Medium when `c_surf` high | Word-wise add; careful with `unsigned` saturation/shift tail |
| M4 | **Delta-light surface blocks (ASM-style)** | `R_DrawSurfaceBlock8_*` | Medium | FIXME already; easy to off-by-one light |
| M5 | **Batch Z and color spans / better span coalescing** | edge emit | Medium | Changes span fragmentation → cache behavior; verify overdraw |
| M6 | **Prefetch next texture row / next span dest** | span drawers | Small–medium | Arch-specific; measure or drop |
| M7 | **`R_InsertNewEdges` binary-friendlier search** | `r_edge.c` | Medium | Easy to break AET order → holes/z-fight |
| M8 | **Surfcache sizing / retention tuning** | `d_surf.c` | Medium | Fewer `R_DrawSurface` calls; memory tradeoff |
| M9 | **Optional SSE2 for `D_DrawZSpans` dual-write & solid fill** | `d_scan.c` / `D_DrawSolidSurface` | Medium | Keep scalar fallback; strict store width/alignment |

### HIGH risk (large win possible, easy to break demos/visuals)

| ID | Candidate | Where | Expected gain | Notes |
|----|-----------|-------|---------------|-------|
| H1 | **SIMD perspective spaners (fixed-point batches)** | `D_DrawSpans8` | **Very high** | Hard: clamps, non-pow2 widths, exact palette indices |
| H2 | **Rewrite edge raster (bucketed AET, SoA edges)** | `r_edge.c` | High | Core visibility; desync = missing walls |
| H3 | **Approximate / quantized lighting** | `r_surf.c` / dlights | High | Changes `c_surf` look; not vanilla |
| H4 | **Drop or defer Z for fully opaque front-to-back world** | Z path | High | Classic already draws world without Z compare then enables Z; further games are fragile |
| H5 | **Reintroduce hand ASM / NASM ports of `d_draw16.s` etc.** | new asm | High on paper | 64-bit ABI, PIC, maintenance; usually worse ROI than good C+SIMD |
| H6 | **Change mip thresholds / always higher mip** | `d_init.c` scalemip | High FPS | Visible blur; gameplay/preference |

---

## 5. Suggested attack order (if optimizing later)

1. **Measure** at target resolution: `r_dspeeds 1`, `perf record -g`, note `%` in `D_DrawSpans8` vs `R_DrawSurfaceBlock*` vs `VID_Update`.  
2. **L1, L2, L3, L8** — cheap structural wins.  
3. **M1 (`D_DrawSpans16` C)** — best classic-faithful fill-rate win missing on x86_64.  
4. **M3/M4** if `c_surf` is high under `r_speeds`.  
5. Only then consider SIMD spaners (H1) with a pixel-diff harness against unoptimized timedemo frames.

---

## 6. File → hotspot map (focus list)

| File | Hot symbols / notes |
|------|---------------------|
| `d_scan.c` | **`D_DrawSpans8`**, **`D_DrawZSpans`**, `Turbulent8`, `D_DrawTurbulent8Span`, `D_WarpScreen` |
| `d_edge.c` | **`D_DrawSurfaces`**, mip select, bmodel rotate thrash, dispatch to spans |
| `d_polyse.c` | **`D_PolysetDrawSpans8`**, gradients, alias raster |
| `d_sprite.c` | `D_SpriteDrawSpans`, `D_DrawSprite` |
| `d_part.c` | `D_DrawParticle` |
| `d_init.c` | `d_drawspans` selection (always 8 on x64) |
| `d_surf.c` | **`D_CacheSurface`**, surfcache rover |
| `r_surf.c` | **`R_DrawSurface`**, **`R_BuildLightMap`**, **`R_DrawSurfaceBlock8_mip*`**, dlights |
| `r_edge.c` | **`R_ScanEdges`**, insert/step/generate spans |
| `r_bsp.c` | **`R_RecursiveWorldNode`**, `R_RenderWorld` |
| `r_main.c` | Frame graph, `R_EdgeDrawing`, entity lists |
| `r_draw.c` | **`R_EmitEdge`**, **`R_RenderFace`**, clipping |
| `r_light.c` | `R_PushDlights`, `R_MarkLights`, `RecursiveLightPoint` |
| `r_aclip.c` | Alias clip (usually modest vs fill) |

---

## 7. Non-goals / reminders

- **glquake** dynlights and GL texture upload are irrelevant to `quake.x11` FPS.
- Correctness work (ASan, UBSan, `-fwrapv` in debug) can **hide** release performance; profile **release** `-O3` binaries.
- Any span/edge change needs **demo1 timedemo stability** and preferably frame MD5/screenshot diffs at fixed `host_framerate` / timedemo.

---

*End of explore report. No code was modified.*
