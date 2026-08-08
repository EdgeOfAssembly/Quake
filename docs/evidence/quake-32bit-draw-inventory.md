# WinQuake software renderer: 8-bit → 24/32-bit draw inventory

**Scope:** `/home/wizard/Quake/WinQuake` (software / `quake.x11` path)  
**Date:** 2026-08-08  
**Mode:** read-only inventory (no code edits)

## Current architecture (baseline)

| Item | Today |
|------|--------|
| `r_pixbytes` | Forced to **1** in `D_Init` (`d_init.c:60`); global default `r_main.c:36` |
| `pixel_t` | `typedef byte pixel_t` (`vid.h:26`) — always 1 byte |
| Framebuffer | X11 TrueColor: `vid.buffer` = `XImage->data`, `vid.rowbytes` = `bytes_per_line` (typically `width*4`) |
| Draw model | Engine draws **8-bit palette indices** into the **start** of each scanline |
| Present | `VID_Update` → `st3_fixup` expands 8→32 in-place via `st2d_8to24table` (`vid_x.c:225–269`, called `1118–1120`, `1142–1145`) |
| `screenwidth` | Set to **`vid.rowbytes`** (byte stride), not pixel width (`d_init.c:134`) |
| `d_scantable[y]` | `y * rowbytes` (`d_modech.c:100`) — byte offset to scanline start |
| Hires textures | `texture_t.rgba` / `rgba_width` / `rgba_height` filled by `Mod_TryLoadExternalRGBA` (`model.c:370–463`); software still builds 8-bit mips and draws them |

**Critical stride fact:**  
`pdest = d_viewbuffer + screenwidth*v + u` treats `u` as a **byte** offset. That is correct only while drawing 1 bpp into the left edge of a wider XImage row. Native 32-bit requires either:

- **A)** keep `screenwidth` = byte stride and use `+ u * r_pixbytes`, or  
- **B)** set `screenwidth` = pixel width and use `uint32_t *` + separate `vid.rowbytes` for padding.

Option A matches existing `d_scantable` / `screenwidth = vid.rowbytes` with minimal semantic change.

---

## 1. Every writer of `d_viewbuffer` / `vid.buffer` assuming 1 byte/pixel

### 1.1 World / span path (`d_viewbuffer` + `screenwidth`)

| File | Function | Lines | Write pattern |
|------|----------|-------|---------------|
| `d_scan.c` | `D_DrawSpans8` | 257–383 | `pdest = d_viewbuffer + screenwidth*v + u`; `*pdest++ = *(pbase + s + t*cachewidth)` (372) |
| `d_scan.c` | `D_DrawSpans16` | 393–507 | same (412–413, 496) |
| `d_scan.c` | `D_DrawSpans32` | 517–630 | same (536–537, 619) — **32 = subdiv width, not bpp** |
| `d_scan.c` | `Turbulent8` | 122–… | `r_turb_pdest = d_viewbuffer + screenwidth*v + u` (142–143); `D_DrawTurbulent8Span` writes `*r_turb_pdest++` (100–112) |
| `d_scan.c` | `D_WarpScreen` | 44–90 | reads `d_viewbuffer` via `rowptr` (64–65); writes `vid.buffer` byte-wise (75–87) |
| `d_edge.c` | `D_DrawSolidSurface` | 83–115 | `pdest = d_viewbuffer + screenwidth*v`; byte fill + dword splat of replicated 8-bit color (89–112) |
| `d_edge.c` | `D_DrawSurfaces` | 175–331 | dispatches sky / turb / solid / `(*d_drawspans)` — all 8-bit today |
| `d_sky.c` | `D_DrawSkyScans8` | 65–137 | `pdest = d_viewbuffer + screenwidth*v + u` (77–78); `*pdest++ = r_skysource[...]` (126–127) |
| `d_sprite.c` | `D_SpriteDrawSpans` | 37–189 | `pdest = d_viewbuffer + screenwidth*v + u` (62); `*pdest = btemp` (169) |
| `d_part.c` | `D_DrawParticle` | 55–204 | `pdest = d_viewbuffer + d_scantable[v] + u` (88); `pdest[i] = color` (108, 121, …) |
| `d_zpoint.c` | `D_DrawZPoint` | ~38 | `pdest = d_viewbuffer + d_scantable[v] + u` |
| `d_polyse.c` | `D_PolysetDrawFinalVerts` | 150–175 | `d_viewbuffer[d_scantable[y]+x] = pix` (171) |
| `d_polyse.c` | recursive triangle | ~380 | same (380) |
| `d_polyse.c` | `D_PolysetScanEdges` / span setup | 767–769, 866 | `d_pdest = d_viewbuffer + y*screenwidth + u` |
| `d_polyse.c` | `D_PolysetRecursiveDrawLine` | 1065–1073 | `d_viewbuffer[ofs] = pix` |

**Setup (not writers, but stride owners):**

| File | Function | Lines | Role |
|------|----------|-------|------|
| `d_init.c` | `D_SetupFrame` | 122–160 | `d_viewbuffer = vid.buffer` or `r_warpbuffer`; `screenwidth = vid.rowbytes` or `WARP_WIDTH`; picks `d_drawspans` |
| `d_modech.c` | `D_ViewChanged` | 59–106 | `d_scantable[i] = i * rowbytes` |
| `d_vars.c` | globals | 42–44 | `cacheblock`, `cachewidth`, `d_viewbuffer` |
| `r_main.c` | `R_RenderView_` | 955–957 | `r_warpbuffer` is `byte[WARP_WIDTH*WARP_HEIGHT]` — **8-bit only**, too small for 32-bit warp |

### 1.2 2D / UI path (`vid.buffer` / `vid.conbuffer` + `vid.rowbytes`)

| File | Function | Lines | Notes |
|------|----------|-------|-------|
| `draw.c` | `Draw_Character` | 136–224 | `r_pixbytes==1` → byte; else 16-bit via `d_8to16table` |
| `draw.c` | `Draw_Pic` | 290–332 | same |
| `draw.c` | `Draw_TransPic` | 340–419 | same |
| `draw.c` | `Draw_TransPicTranslate` | 427–506 | same |
| `draw.c` | `Draw_ConsoleBackground` | 569–621 | same |
| `draw.c` | `R_DrawRect8` | 630–671 | always 8-bit dest |
| `draw.c` | `R_DrawRect16` | 679–729 | 16-bit dest |
| `draw.c` | `Draw_TileClear` | 740–802 | branches 1 vs 16 |
| `draw.c` | `Draw_Fill` | 812–835 | branches 1 vs 16 |
| `draw.c` | `Draw_FadeScreen` | 844–869 | **always 8-bit** `pbuf[x]=0` (no `r_pixbytes` branch) |
| `d_fill.c` | `D_FillRect` | 30–87 | **always 8-bit** dword-replicated color |
| `r_misc.c` | `R_LineGraph` | 111–139 | **always 8-bit** |
| `screen.c` | screenshot PCX | ~644 | dumps `vid.buffer` as 8-bit PCX |
| `view.c` | LCD stereo hack | 1030–1054 | mutates `vid.rowbytes` / `vid.buffer` |

### 1.3 Present / expand (X11)

| File | Function | Lines | Role |
|------|----------|-------|------|
| `vid_x.c` | `st2_fixup` | 189–223 | in-place 8→16 |
| `vid_x.c` | `st3_fixup` | 225–269 | in-place 8→24/32 via `st2d_8to24table` |
| `vid_x.c` | `VID_SetPalette` | 738–764 | fills `st2d_8to16table` / `st2d_8to24table` |
| `vid_x.c` | `VID_Update` | 1072–1153 | calls fixup when `depth==16` or `24`, then `XShmPutImage` / `XPutImage` |

**Note:** Software X11 uses **static** `st2d_8to24table`; global `d_8to24table` in `vid.h:58` is populated by **GL** drivers, not `vid_x.c`. Native 32-bit draw should publish a shared `d_8to24table` (or export `st2d`) for particles/UI.

### 1.4 Surface cache producers (not framebuffer, but feed span drawers)

| File | Function | Lines | Output |
|------|----------|-------|--------|
| `d_surf.c` | `D_CacheSurface` | 264–333 | allocates cache; `r_drawsurf.surfdat = cache->data`; calls `R_DrawSurface` |
| `r_surf.c` | `R_DrawSurface` | 248–331 | lights texture into cache (8 or incomplete 16) |
| `r_surf.c` | `R_DrawSurfaceBlock8_mip0..3` | 343–… | 8-bit lit texels via `vid.colormap` |
| `r_surf.c` | `R_DrawSurfaceBlock16` | 546–584 | incomplete 16-bit (`FIXME: make this work`); uses `vid.colormap16` |

---

## 2. Every `r_pixbytes` branch (1 vs 2 vs other)

### 2.1 Assignment / default

| Location | Lines | Behavior |
|----------|-------|----------|
| `r_main.c` | 36 | `int r_pixbytes = 1;` |
| `d_init.c` `D_Init` | 60 | **`r_pixbytes = 1;` always** — overrides any driver intent |
| `d_iface.h` | 129 | `extern int r_pixbytes;` |

### 2.2 Branches that exist today

| File | Function | Lines | `== 1` | else / `== 2` | other |
|------|----------|-------|--------|---------------|-------|
| `draw.c` | `Draw_Character` | 169–223 | 8-bit conbuffer | 16-bit `d_8to16table` | — |
| `draw.c` | `Draw_Pic` | 305–331 | memcpy 8 | expand 16 | — |
| `draw.c` | `Draw_TransPic` | 354–418 | 8 | 16 | — |
| `draw.c` | `Draw_TransPicTranslate` | 441–505 | 8 | 16 (ignores translation in 16 path!) | — |
| `draw.c` | `Draw_ConsoleBackground` | 569–621 | 8 | 16 | — |
| `draw.c` | `Draw_TileClear` | 784–791 | `R_DrawRect8` | `R_DrawRect16` | — |
| `draw.c` | `Draw_Fill` | 819–834 | 8 | 16 | — |
| `r_surf.c` | `R_DrawSurface` | 284–295 | `surfmiptable` + `horzblockstep=blocksize` | `R_DrawSurfaceBlock16` + `horzblockstep=blocksize<<1` | — |
| `r_surf.c` | `R_GenTile` | 652–672 | turb/sky 8 | turb/sky 16 | — |
| `r_main.c` | view change / ASM patch | 466–479 | `R_Surf8Patch` | `R_Surf16Patch` | id386 only |
| `model.c` | `Mod_LoadAliasSkin` | 1596–1610 | memcpy | expand `d_8to16table` | **`Sys_Error` if not 1 or 2** |
| `model.c` | `Mod_LoadSpriteFrame` | 1948–1963 | memcpy | expand 16 | **`Sys_Error` if not 1 or 2** |

### 2.3 Gaps (no `r_pixbytes == 4` path)

- All span drawers (`D_DrawSpans*`, turb, sky, sprite, particle, alias)
- `D_FillRect`, `Draw_FadeScreen`, `R_LineGraph`
- `D_SCAlloc` / `D_CacheSurface` size math
- `r_warpbuffer` sizing
- `Mod_Load*` rejects `r_pixbytes == 4`
- `VID_Update` always runs `st3_fixup` for depth 24 regardless of native draw

### 2.4 Historical 16-bit design notes (incomplete)

- `draw.c` treats non-1 as **16-bit** (`unsigned short`, `rowbytes>>1`).
- `R_DrawSurfaceBlock16` marked **FIXME**; `surfrowbytes` still set to **texel** width (`d_surf.c:296`), not `width * r_pixbytes` — 16-bit surface cache was never finished.
- No working 32-bit software path exists; only GL uses `d_8to24table`.

---

## 3. Surface cache allocation

### 3.1 Heap sizing

```
D_SurfaceCacheForRes(width, height)   d_surf.c:35–53
  base = SURFCACHE_SIZE_AT_320X200 (600*1024)
  if pix > 64000: size += (pix-64000)*3
  // 8-bit oriented; 32-bit needs ~×4 (or ×r_pixbytes)
```

Allocated in `vid_x.c` `ResetFrameBuffer` / `ResetSharedFrameBuffers` (~336–346, ~400–410) then `D_InitCaches`.

### 3.2 `D_SCAlloc` (`d_surf.c:130–209`)

| Check | Limit | 32-bit impact |
|-------|-------|----------------|
| `width` | 0…256 | width is **texels** (OK) |
| `size` | 1…**0x10000** (64 KiB) | **blocks** 256×256×4 = 256 KiB — **must raise** |
| header | `size = offsetof(data[size])` rounded to 4 | OK if `size` is byte payload |
| height debug | `data_bytes / width` | wrong if width=texels and data is 32-bit (DEBUG only) |

### 3.3 `D_CacheSurface` (`d_surf.c:264–333`)

```
r_drawsurf.surfwidth  = extents[0] >> miplevel;     // texels
r_drawsurf.rowbytes   = r_drawsurf.surfwidth;       // ← BYTE stride today (= texels for 8-bit)
r_drawsurf.surfheight = extents[1] >> miplevel;
cache = D_SCAlloc(surfwidth, surfwidth * surfheight);  // ← bytes = texels for 8-bit
r_drawsurf.surfdat = cache->data;
R_DrawSurface();
```

**For 32-bit MVP:**

```
rowbytes = surfwidth * r_pixbytes;
D_SCAlloc(surfwidth, surfwidth * surfheight * r_pixbytes);
```

And `R_DrawSurface` / block drawers must write `uint32_t` with `horzblockstep = blocksize * r_pixbytes`, `prowdest += surfrowbytes` in **bytes**.

### 3.4 Span sampling of cache

`cacheblock = cache->data`, `cachewidth = cache->width` (**texels**).  
Index: `pbase[(s>>16) + (t>>16)*cachewidth]` — for 32-bit, `pbase` must be `uint32_t *` (or multiply index by 4).

### 3.5 `texture_t.rgba` (hires source)

```
model.h:77–81   byte *rgba; int rgba_width, rgba_height;
model.c:370–463 Mod_TryLoadExternalRGBA — load .tga/.rgba, keep full-res RGBA on hunk,
                still quantize to 8-bit mips for current draw
```

**Lit RGB surface cache goal:** when `mt->rgba` present, `R_DrawSurface` (or new `R_DrawSurfaceBlock32`) samples RGBA (mip via box/nearest from full-res), multiplies by lightmap (`blocklights`), packs to X11 pixel format (`xlib_rgb24` / `d_8to24table`-compatible), stores in cache. Fallback: 8-bit mip + `d_8to24table[colormap[light+pix]]`.

---

## 4. Recommended implementation order

### Phase 0 — Plumbing (no visible change if still drawing 8-bit)

1. **`r_pixbytes` policy**  
   - Stop forcing `=1` in `D_Init`, or set after VID init.  
   - In `vid_x.c` after visual known: `r_pixbytes = (depth >= 24) ? 4 : (depth == 16 ? 2 : 1)`.  
   - Publish `d_8to24table[256]` from `VID_SetPalette` (same packing as `st2d_8to24table` / `xlib_rgb24`).

2. **Stride convention (document & enforce)**  
   - Keep `screenwidth = vid.rowbytes` (byte stride).  
   - All horizontal offsets: `u * r_pixbytes`.  
   - `d_scantable[y] = y * rowbytes` stays valid.

3. **Surface cache**  
   - `D_SurfaceCacheForRes` × `r_pixbytes`.  
   - `D_SCAlloc` max size ≥ largest lit surface × 4 (e.g. 0x40000+).  
   - `D_CacheSurface` size/rowbytes × `r_pixbytes`.

### Phase 1 — MVP (world visible in native 32-bit)

**Goal:** world brushes + lightmapped surfaces + present without `st3_fixup`.

| Step | What | Primary files |
|------|------|----------------|
| 1a | `R_DrawSurface` / new `R_DrawSurfaceBlock32*` — lit 32-bit cache (rgba or 8→24) | `r_surf.c`, `d_surf.c` |
| 1b | `D_DrawSpans32bpp` (name TBD) — sample `uint32_t` cache → framebuffer | `d_scan.c`, `d_init.c` (`d_drawspans`) |
| 1c | `D_DrawSolidSurface` 32-bit fill (background / r_drawflat) | `d_edge.c` |
| 1d | Skip `st3_fixup` when `r_pixbytes==4` | `vid_x.c` `VID_Update` |
| 1e | Smoke: `+map e1m1` walls correct color, no expand pass | |

**Out of MVP (still 8-bit or wrong until later):** sky, water turb, particles, alias models, sprites, console/HUD, warp water view, fade screen.

Optional interim: keep drawing 8-bit world but skip fixup only after full conversion — **do not** skip fixup until all pixels written to rects are native 32-bit (or clear FB to 32-bit first).

### Phase 2 — Completeness of 3D

| Order | Path | Files |
|-------|------|-------|
| 2a | Sky `D_DrawSkyScans8` → 32 | `d_sky.c`, `r_sky.c` |
| 2b | Turbulent water/slime | `d_scan.c` `Turbulent8` |
| 2c | Particles | `d_part.c` — `d_8to24table[color]` |
| 2d | Sprites | `d_sprite.c`, `model.c` `Mod_LoadSpriteFrame` allow pixbytes 4 |
| 2e | Alias models | `d_polyse.c`, `model.c` `Mod_LoadAliasSkin` |
| 2f | Warp buffer | `r_main.c` size `WARP_WIDTH*WARP_HEIGHT*r_pixbytes`; `D_WarpScreen` |

### Phase 3 — 2D / console / UI

| Order | Path | Files |
|-------|------|-------|
| 3a | `Draw_*` add `r_pixbytes==4` (mirror 16-bit branches with `uint32_t` + `d_8to24table`) | `draw.c` |
| 3b | `D_FillRect`, `Draw_FadeScreen`, `R_LineGraph` | `d_fill.c`, `draw.c`, `r_misc.c` |
| 3c | Screenshot (PNG/truecolor or quantize) | `screen.c` |

### Phase 4 — Polish

- `r_warpbuffer` / water warp correctness  
- Fullbright / colormap edge cases in RGB lighting  
- Raise surface cache under thrash; timedemo  
- ASan/UBSan: `make sanitize-x11`  
- Do **not** rely on unfinished 16-bit path; prefer explicit `1 / 4` (and optional 2 later)

---

## 5. Exact function names and line numbers — MVP path

### 5.1 Enable native 32-bit mode

| Action | File:lines |
|--------|------------|
| Remove/guard force-1 | `d_init.c` `D_Init` **:60** |
| Global default | `r_main.c` **:36** |
| Set from X depth | `vid_x.c` after visual (e.g. near `VID_Init` ~495–498 / mode set ~718–725) |
| Palette → 32-bit LUT | `vid_x.c` `VID_SetPalette` **:738–747** (extend to `d_8to24table`) |
| Skip expand | `vid_x.c` `VID_Update` **:1113–1120**, **:1138–1145** — only call `st3_fixup` if `r_pixbytes==1` |

### 5.2 Frame setup / strides

| Function | File:lines | Change |
|----------|------------|--------|
| `D_SetupFrame` | `d_init.c:122–160` | keep `screenwidth=vid.rowbytes`; select 32-bit span drawer when `r_pixbytes==4` |
| `D_ViewChanged` | `d_modech.c:59–106` | `d_scantable` OK if still byte offsets |
| `D_DrawSurfaces` | `d_edge.c:175–331` | solid/sky/turb later; world uses `d_drawspans` **:308** |

### 5.3 Surface cache (MVP core)

| Function | File:lines | Change |
|----------|------------|--------|
| `D_SurfaceCacheForRes` | `d_surf.c:35–53` | scale by `r_pixbytes` |
| `D_SCAlloc` | `d_surf.c:130–209` | raise `size > 0x10000` limit (**:138–139**) |
| `D_CacheSurface` | `d_surf.c:264–333` | **:296–305** `rowbytes` & alloc size × `r_pixbytes` |
| `R_DrawSurface` | `r_surf.c:248–331` | **:284–295** add `r_pixbytes==4` drawer |
| `R_DrawSurfaceBlock8_mip0` (template) | `r_surf.c:343–386` | clone → Block32: write `uint32_t`, light×RGB |
| `R_BuildLightMap` | `r_surf.c` ~159+ | keep; feed RGB multiply |
| `texture_t.rgba` | `model.h:77–81`, filled `model.c:458–460` | sample when non-NULL |

### 5.4 Span blit cache → framebuffer (MVP core)

| Function | File:lines | Change |
|----------|------------|--------|
| `D_DrawSpans8/16/32` | `d_scan.c:257–630` | new 32-**bit** drawer: `pdest` as `uint32_t *` or byte* + `u*4`; sample `uint32_t` cache |
| Dest address pattern | e.g. **:276–277**, **:412–413**, **:536–537** | `+ pspan->u * r_pixbytes` |
| Pixel store | e.g. **:372**, **:496**, **:619** | 32-bit store |
| `d_drawspans` select | `d_init.c:152–158` | if `r_pixbytes==4` → 32bpp drawer (independent of subdiv 8/16/32) |

### 5.5 Solid fills used by world path

| Function | File:lines |
|----------|------------|
| `D_DrawSolidSurface` | `d_edge.c:83–115` — background / drawflat |
| Call sites | `d_edge.c:199`, **:234** |

### 5.6 Present

| Function | File:lines |
|----------|------------|
| `VID_Update` | `vid_x.c:1072–1153` |
| `st3_fixup` (skip when native) | `vid_x.c:225–269` |

### 5.7 Explicitly **not** MVP (but will show wrong until Phase 2/3)

| Function | File:lines | Symptom if left 8-bit |
|----------|------------|------------------------|
| `D_DrawSkyScans8` | `d_sky.c:65–137` | sky garbage / dark |
| `Turbulent8` | `d_scan.c:122+` | water wrong |
| `D_DrawParticle` | `d_part.c:55–204` | particles wrong |
| `D_SpriteDrawSpans` | `d_sprite.c:37–189` | sprites wrong |
| `D_Polyset*` | `d_polyse.c:171,380,767,1073` | models wrong |
| `Draw_Character` / HUD | `draw.c:136+` | console/HUD wrong |
| `D_WarpScreen` | `d_scan.c:44–90` | underwater warp wrong |
| `r_warpbuffer` | `r_main.c:955–957` | 8-bit 320× buffer |

### 5.8 Model load guards (block `r_pixbytes=4` until Phase 2)

| Function | File:lines |
|----------|------------|
| `Mod_LoadAliasSkin` | `model.c:1596–1610` |
| `Mod_LoadSpriteFrame` | `model.c:1948–1963` |

For MVP with `r_pixbytes=4` **before** alias/sprite conversion: either keep loading skins as 8-bit (ignore pixbytes for skins until Phase 2) or avoid drawing alias/sprites (they will error on load if strict).

**MVP recommendation:** allow `r_pixbytes==4` in model load by still storing **8-bit** skins/sprites (treat like pixbytes 1 for asset storage) until Phase 2 expands them; only surface cache + spans + VID are 32-bit. That avoids `Sys_Error` without full alias work.

---

## 6. Quick reference: data flow (MVP)

```
texture_t (8-bit mips and/or .rgba)
        │
        ▼
R_DrawSurface / Block32  ──►  surfcache_t.data  (lit RGBX uint32, rowbytes = width*4)
        │
        ▼
D_CacheSurface  ◄── D_SCAlloc(width, width*height*4)
        │
        ▼
D_DrawSurfaces → d_drawspans  ──►  d_viewbuffer / vid.buffer  (native 32-bit, stride vid.rowbytes)
        │
        ▼
VID_Update  ──►  XShmPutImage  (NO st3_fixup when r_pixbytes==4)
```

---

## 7. Risk checklist

| Risk | Detail |
|------|--------|
| `D_SCAlloc` 64K cap | Hard fail on large 32-bit surfaces |
| Surface cache thrash | 4× memory; bump `D_SurfaceCacheForRes` |
| `screenwidth` vs pixels | Easy to miss a `+ u` without `* r_pixbytes` |
| `pixel_t` still `byte` | Keep casts; don't change typedef globally without audit |
| Mixed rects | If HUD still 8-bit while world 32-bit, fixup can't run on whole rect |
| Warp buffer | Stack 320×200 bytes — overflow if written as 32-bit |
| Incomplete 16-bit code | Don't extend; add explicit 4-byte path |
| `Draw_FadeScreen` / `D_FillRect` | No branch — will corrupt 32-bit FB |
| Endian / channel order | Must match `xlib_rgb24` / X visual masks |

---

## 8. File touch list (MVP only)

| Priority | File |
|----------|------|
| P0 | `d_init.c`, `vid_x.c` |
| P0 | `d_surf.c`, `r_surf.c` |
| P0 | `d_scan.c`, `d_edge.c` |
| P1 (guards) | `model.c` (allow load with pixbytes 4 without expanding skins) |
| Later | `d_sky.c`, `d_part.c`, `d_sprite.c`, `d_polyse.c`, `draw.c`, `d_fill.c`, `r_main.c`, `d_modech.c` (if needed) |

---

*End of inventory.*
