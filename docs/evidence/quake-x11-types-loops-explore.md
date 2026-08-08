# quake.x11 integer types & for-loop hygiene explore

**Scope:** `/home/wizard/Quake/WinQuake` — focus files for software X11 path  
**Policy:** prefer `stdint.h` / `q_stdint.h`; `size_t` loop indexes; `const size_t` length outside loop; `ssize_t` if index can be negative; hoist invariants; flag overflow (`count*size`, `end-start`, `int` buffer sizes)  
**Date:** 2026-08-08  
**Mode:** read-only audit (no code edits)

## Already done (context)

| Item | Status |
|------|--------|
| `q_stdint.h` | Present: `qint32_t`, `qintptr_t`, `qsize_t`, etc. |
| QC `string_t` / `func_t` | `int32_t` in `pr_comp.h` (LP64-clean) |
| `STRUCT_FROM_LINK` | Uses `ptrdiff_t` / `offsetof` (was `int`) |
| `PIXEL24` in `vid_x.c` | Fixed to `uint32_t` (was `unsigned long` → SHM overrun) |

---

## 1. Worst overflow risks (file:line)

Ranked by exploitability / crash likelihood under large or hostile inputs (pak/BSP/mdl) or high video modes.

### CRITICAL — allocation size wrap → undersized hunk → heap smash

| Location | Pattern | Why it hurts |
|----------|---------|--------------|
| **`model.c:389`** | `pixels = mt->width*mt->height/64*85` then `Hunk_AllocName(sizeof(texture_t)+pixels)` | All `int`. Hostile miptex width/height (from BSP) can wrap `pixels` to a small value; `memcpy(tx+1, mt+1, pixels)` still uses that size, but later rendering assumes full texture. Classic integer overflow → OOB. |
| **`model.c:565`** (and same pattern ~592, 629, 658, 777, 860, 907, 954, 1009, 1045, 1073, …) | `count = l->filelen / sizeof(*in); out = Hunk_AllocName(count*sizeof(*out), …)` | `filelen` is map-controlled. `count * sizeof(*out)` is **signed `int` multiply**. If product exceeds `INT_MAX`, wraps; `Hunk_AllocName` gets a tiny size; load loop writes `count` elements → **heap overflow**. |
| **`model.c:1471–1475`** | Alias header size = sum of `LittleLong(numframes/numverts/numtris) * sizeof(...)` | No checked multiply/add. Malicious `.mdl` can request huge `size` that wraps before `Hunk_AllocName`. |
| **`model.c:1681–1684`** | `size = width * height;` then `Hunk_AllocName(sizeof(mspriteframe_t) + size*r_pixbytes)` | Sprite frame dims from file; `width*height` and `size*r_pixbytes` both unchecked `int`. |
| **`zone.c:410`** / **`zone.c:499`** | `size = sizeof(hunk_t) + ((size+15)&~15)` | If caller passes `size` near `INT_MAX`, `size+15` wraps → tiny allocation, caller thinks it got full buffer. Same class in **`zone.c:167–169`** (`Z_TagMalloc`) and **`zone.c:881`** (`Cache_Alloc`). |
| **`common.c:1554–1564`** | `Hunk_AllocName(len+1, …)` / `Z_Malloc(len+1)` | `len` from pack/file. `len == INT_MAX` → `len+1` wraps to 0/negative path. |

### HIGH — video path (quake.x11)

| Location | Pattern | Why it hurts |
|----------|---------|--------------|
| **`vid_x.c:330–336`** / **`385–391`** | `X11_buffersize = vid.width * vid.height * sizeof(*d_pzbuffer);` then `+= vid_surfcachesize`; pass to `Hunk_HighAllocName(int)` | `vid.width`/`height` are `unsigned`; product with `sizeof` can still overflow **32-bit intermediate** if stored/truncated through `int` API. `X11_buffersize` is **`long`** (OK on LP64) but **`Hunk_HighAllocName` takes `int`** → silent truncation on huge modes. |
| **`vid_x.c:347`** | `mem = ((vid.width*pwidth+7)&~7) * vid.height;` then `malloc(mem)` | Same class for XImage backing store; `int mem`. FullHD is fine; pathological modes are not. |
| **`vid_x.c:341`** / **`396`** | `vid_surfcache = (byte*)d_pzbuffer + vid.width * vid.height * sizeof(*d_pzbuffer)` | If `X11_buffersize` was truncated but this product is computed differently, cache pointer can land outside allocated region. |

### MEDIUM — buffer / network / lightmap

| Location | Pattern | Why it hurts |
|----------|---------|--------------|
| **`common.c:761–775`** | `SZ_GetSpace`: `buf->cursize + length > buf->maxsize` | If `cursize + length` overflows `int`, check fails open → write past `sizebuf`. |
| **`common.c:1653`** | `numpackfiles * sizeof(packfile_t)` | Mitigated by `MAX_FILES_IN_PACK` check (1647); still should use checked multiply for hygiene. |
| **`r_surf.c:163`** | `size = smax * tmax` with `smax/tmax` from surface extents | Turb surfaces force extents 16384 (`model.c:822–823`) → large but usually OK; still `int` product into `blocklights[i]` loops. |
| **`net_dgrm.c:360–389`** | `length = BigLong(...); length &= NETFLAG_LENGTH_MASK; length -= NET_HEADERSIZE; SZ_Write(..., length)` | Length is masked (good). Ensure `length >= NET_HEADERSIZE` before subtract (partially gated by earlier `length < NET_HEADERSIZE` on **wire** size, not on decoded field — verify both paths). Prefer `uint32_t` end-to-end. |

### Lower priority (fixed-bound or cosmetic)

- **`d_scan.c` / `d_edge.c`**: span/screen loops use `int` for pixel coords; bounds are screen-sized (`MAXWIDTH`/`MAXHEIGHT`). Overflow risk is low unless those constants grow past 32k without audit.
- **`world.c`**: mostly `i < 3` / `i < 6` and pointer-chasing lists; not size-arithmetic heavy.
- **`pr_exec.c`**: locals/stack sizes come from progs; stack overflow is checked (`MAX_STACK_DEPTH`, `LOCALSTACK_SIZE`); index type is hygiene, not primary overflow.

---

## 2. Loops that should use `size_t` / `ssize_t` (examples)

Policy reminder:

```c
const size_t n = count;
for (size_t i = 0; i < n; i++)
    ...
/* descending / can go negative: */
for (ssize_t b = 15; b >= 0; b--)
    ...
```

### High value (byte counts / file-driven counts)

| File:line | Current | Suggested |
|-----------|---------|-----------|
| **`common.c:138–167`** `Q_memset` / `Q_memcpy` | `int i`, `int count` | `size_t count`; `size_t i`; alignment via `uintptr_t` not `(long)` |
| **`common.c:1660–1666`** pack CRC + directory | `for (i=0; i<header.dirlen; i++)`, `for (i=0; i<numpackfiles; i++)` | `const size_t dirlen = …; const size_t nfiles = …` |
| **`common.c:1399`** pak file search | `for (i=0; i<pak->numfiles; i++)` | `size_t` over `numfiles` |
| **`model.c:570+`** all `Mod_Load*` lump loops | `int i, count; for (i=0; i<count; i++)` | `size_t i`; `const size_t count = …` after checked divide |
| **`model.c:376–408`** miptex loops | `for (i=0; i<m->nummiptex; i++)` | `size_t` if `nummiptex` promoted |
| **`pr_exec.c:309–317`**, **`346–347`** | `for (i=0; i<c; i++)` locals copy | `size_t` with `c` as `size_t` (bounds already checked) |
| **`r_surf.c:168–194`** | `for (i=0; i<size; i++)` lightmap | `const size_t size = …; for (size_t i = 0; i < size; i++)` |

### Medium value (screen / block loops — hoist lengths)

| File:line | Note |
|-----------|------|
| **`vid_x.c:198`**, **`248`** | `for (yi = y; yi < (y+height); yi++)` — hoist `const int y_end = y + height` (or `size_t` if dims unsigned) |
| **`vid_x.c:723`**, **`732`** | palette 0..255 — fine as `size_t i` / keep `int` |
| **`d_scan.c:62–82`** | hoist `const int h = scr_vrect.height`, `w = scr_vrect.width` once; use `size_t` if widths become `unsigned` consistently with `viddef_t` |
| **`d_edge.c:99–111`** | `u`/`u2` are pixel coords; can stay `int`/`ssize_t` (signed span math); avoid mixing with `size_t` blindly |
| **`r_surf.c:315`**, **`351`**, **`368`** | `r_numhblocks`/`r_numvblocks` — hoist; inner `for (b=15; b>=0; b--)` → **`ssize_t b`** |
| **`world.c:78`**, **`395`**, etc. | fixed `i<3`/`i<6` — low priority; `size_t` optional |

### Alignment / pointer-as-integer anti-patterns inside loops

| File:line | Issue |
|-----------|--------|
| **`common.c:142`**, **`158`** | `((long)dest \| count) & 3` — use `(uintptr_t)dest` |
| **`r_surf.c:579`** | `prowdest = (unsigned short *)((long)prowdest + surfrowbytes)` — use `(byte *)prowdest + surfrowbytes` or `uintptr_t` |

---

## 3. Bare `long` / `int` size fields that are LP64-dangerous

### Dangerous or inconsistent on LP64

| Symbol | File:line | Type today | Risk |
|--------|-----------|------------|------|
| `X11_highhunkmark` | `vid_x.c:100` | `static long` | Filled from `Hunk_HighMark()` → **`int`**. On LP64 `long` is wider (harmless) but **wrong abstraction**; should match hunk mark type (`int` or future `size_t`). |
| `X11_buffersize` | `vid_x.c:101` | `static long` | Computed as pixel product; passed to **`Hunk_HighAllocName(int)`** → truncation. Prefer `size_t` + clamp/check before hunk API. |
| `r_shift,g_shift,b_shift` | `vid_x.c:120` | `static long` | Only need small signed shift counts → `int` / `int32_t`. |
| `r_mask,g_mask,b_mask` | `vid_x.c:121` | `unsigned long` | X11 visual masks; OK as `unsigned long` **or** `unsigned` / `uint32_t` (masks are 32-bit). |
| `(long)dest` alignment | `common.c:142,158` | cast to `long` | Works if pointer fits; **`uintptr_t`** is correct. |
| `(long)prowdest` | `r_surf.c:579` | pointer via `long` | Same; use byte pointer arithmetic. |
| `banAddr` / `banMask` | `net_dgrm.c:102–103` | `unsigned long` | IPv4 addresses → **`uint32_t`**. |
| `S_addr` | `net_dgrm.c:39` | `unsigned long` | Same. |
| Entire zone/hunk API | `zone.h:86–124` | `int size` everywhere | Caps single alloc at ~2 GiB; **all** callers use `int`. Intentional 1990s model, but every `count*sizeof` feeds this. Migration = API + call sites. |
| `hunk_size`, `hunk_low_used`, `hunk_high_used` | `zone.c:276–279` | `int` | Same 2 GiB ceiling; marks are `int`. |
| `memblock_t.size`, `hunk_t.size` | `zone.c:31,271` | `int` | On-disk N/A (runtime only); keep `int32_t` or `size_t` consistently. |
| `sizebuf_t.maxsize/cursize` | `common.h:41–42` | `int` | Network/message sizes; `int32_t` clearer; overflow in `SZ_GetSpace` (above). |
| `com_filesize` | `common.h:166` | `int` | File sizes >2 GiB truncated. |
| `packfile_t` filepos/filelen | (via LittleLong) | typically `int` | Pak format is 32-bit — **`int32_t`** is correct (not `long`). |
| `viddef_t.rowbytes` | `vid.h:40` | `unsigned` | OK; mix with `int conrowbytes` is inconsistent. |
| `model_t` num* fields | `model.h:322+` | `int` | Map counts; `int32_t` matches BSP; not pointer-sized. |

### Not LP64-dangerous (keep 32-bit)

- QC / progs: `string_t`, `func_t`, statement offsets — **must stay `int32_t`**.
- `LittleLong` / `BigLong` function pointers take/return **`int`** (`common.h:95–96`) — on-disk endian helpers; fine if always 32-bit values.
- Pixel coords, clipnode indices, light styles — domain-limited `int` is fine.

### `PIXEL24` — fixed (do not regress)

```c
/* vid_x.c — already correct */
typedef uint32_t PIXEL24;  /* NOT unsigned long */
```

---

## 4. Suggested phased migration (small PRs)

Each phase should be one FEATURE/FIXUP series, build `quake.x11`, smoke `+map e1m1`. Prefer additive helpers over drive-by renames.

### Phase 0 — helpers (no behavior change if unused)

**PR:** add to `q_stdint.h` or small `q_size.h`:

```c
/* return false on overflow */
bool Q_size_mul(size_t a, size_t b, size_t *out);
bool Q_size_add(size_t a, size_t b, size_t *out);
/* int-capped for current hunk API */
int Q_size_to_int(size_t n); /* Sys_Error if > INT_MAX */
```

Optional: `Hunk_AllocName_size(size_t, char*)` wrapper that checks then calls existing `int` API.

### Phase 1 — `vid_x.c` only (quake.x11 path) — **highest playtest value**

1. Replace `static long X11_highhunkmark` → `int` (match `Hunk_HighMark`).
2. Replace `static long X11_buffersize` → `size_t`; compute with checked mul/add; reject modes that don't fit `int` hunk API.
3. `r_shift*` → `int`; masks → `uint32_t` or leave `unsigned long` with comment.
4. Hoist `y+height` in `st2_fixup` / `st3_fixup`.
5. **Do not** touch `PIXEL24`.

**Acceptance:** FullHD/windowed mode; no SHM crash; `Hunk_HighAllocName` never sees truncated size.

### Phase 2 — zone overflow guards (API stays `int` initially)

1. In `Hunk_AllocName` / `Hunk_HighAllocName` / `Z_TagMalloc` / `Cache_Alloc`: reject `size < 0` (already) **and** detect wrap on `size + header + align`.
2. Document max single allocation (`INT_MAX - sizeof(hunk_t) - 15`).
3. No call-site churn yet.

**Acceptance:** unit-style tests or assert path with huge size → clean `Sys_Error`, not silent wrap.

### Phase 3 — `model.c` lump loaders (security-relevant)

1. Introduce local pattern:

   ```c
   if (l->filelen % sizeof(*in)) Sys_Error(...);
   const size_t count = (size_t)l->filelen / sizeof(*in);
   size_t bytes;
   if (!Q_size_mul(count, sizeof(*out), &bytes)) Sys_Error(...);
   out = Hunk_AllocName(Q_size_to_int(bytes), loadname);
   for (size_t i = 0; i < count; i++, in++, out++)
   ```

2. Fix texture `pixels = width*height/64*85` with checked math + max texture clamp.
3. Fix alias (`1471`) and sprite (`1681`) size assembly the same way.

**Acceptance:** normal id1 maps/models load; deliberately corrupt lump sizes error out.

### Phase 4 — `common.c` mem + pack + sizebuf

1. `Q_memset` / `Q_memcpy` / `Q_memcmp`: `size_t count`, `uintptr_t` alignment.
2. `COM_LoadPackFile`: `size_t` for directory walk; checked `numpackfiles * sizeof(packfile_t)`.
3. `COM_LoadFile`: check `len >= 0` and `len < INT_MAX` before `len+1`.
4. `SZ_GetSpace`: use unsigned or checked add for `cursize + length`.

### Phase 5 — renderer loop hygiene (`r_surf.c`, `d_scan.c`, `d_edge.c`)

1. Hoist `r_numvblocks` / `scr_vrect.width` etc. to `const` locals.
2. Descending block loops → `ssize_t`.
3. Kill `(long)prowdest` pointer math.
4. Keep fixed-point / span math as `int` where signed is required.

**Low risk of gameplay change; mostly style + future-proofing.**

### Phase 6 — `net_dgrm.c` / `pr_exec.c` / `world.c` polish

1. IPv4 fields → `uint32_t`.
2. Packet length path: explicit `uint32_t length`, validate before `SZ_Write`.
3. `pr_exec` locals loops → `size_t` (optional).
4. `world.c` leave mostly alone unless touching adjacent code.

### Phase 7 (optional, large) — zone API `size_t`

Only after Phases 1–4 are green: change `Hunk_*` / `Z_*` signatures to `size_t`, update all call sites, keep internal 32-bit limit or raise with care. **Not needed for quake.x11 correctness** if Phase 2 guards exist.

---

## 5. Per-file snapshot (focus list)

| File | Overflow | Loop types | Bare long/int sizes | Priority |
|------|----------|------------|---------------------|----------|
| **zone.c** | Align/header wrap on alloc | Mostly pointer walks | All sizes `int` | P2 |
| **common.c** | pack/load/sizebuf/Q_mem | `int i` everywhere | `(long)` align; `int` sizes | P4 |
| **vid_x.c** | width×height×bpp | screen loops | `long` mark/buffersize | **P1** |
| **model.c** | count×sizeof, tex/alias/sprite | every lump loader | counts as `int` | **P3** |
| **r_surf.c** | smax×tmax | lightmap/block loops | `(long)` ptr | P5 |
| **d_scan.c** | low (screen-bound) | u/v int | counts int | P5 |
| **d_edge.c** | low | span u int | `screenwidth` int | P5 |
| **pr_exec.c** | stack bounds exist | locals `int i` | progs offsets int32 OK | P6 |
| **world.c** | low | fixed small loops | hull nums int OK | P6 |
| **net_dgrm.c** | packet length path | few count loops | `unsigned long` IPv4 | P6 |

---

## 6. Recommended first PR (minimal)

**Title sketch:** `FEATURE v1 vid_x size_t buffers + hunk mark types`

1. `X11_highhunkmark`: `long` → `int`  
2. `X11_buffersize`: `long` → `size_t` + overflow check before `Hunk_HighAllocName`  
3. Optional: `mem` for `malloc` as `size_t`  
4. No model/zone API changes  

**Why first:** isolated to X11 driver; matches prior PIXEL24 LP64 fix; directly protects quake.x11 mode switches.

---

## 7. Out of scope / notes

- GL path (`gl_*.c`) not audited here; same `model` patterns exist in `gl_model.c`.
- ASM (`*.s`) still assumes 32-bit register sizes for some paths; `size_t` C loops must not break `id386` builds without paired ASM review.
- Do not widen QC VM field types to 64-bit.
- Formal verification: not run (explore only). Natural CBMC targets later: `Q_size_mul`, `Hunk_AllocName` wrap checks, `Mod_LoadVertexes` size path.

---

## 8. Evidence sources

- `/home/wizard/Quake/WinQuake/{zone,common,vid_x,model,r_surf,d_scan,d_edge,pr_exec,world,net_dgrm}.c`
- `/home/wizard/Quake/WinQuake/{zone,common,vid,model,progs,pr_comp,q_stdint}.h`
- Grep: `count*sizeof`, `for (`, `\blong\b`, size fields
