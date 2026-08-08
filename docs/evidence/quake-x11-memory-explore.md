# quake.x11 memory-safety explore (WinQuake)

**Scope:** `/home/wizard/Quake/WinQuake` (software X11 / `quake.x11`)  
**Mode:** read-only static review (grep + targeted reads)  
**Date:** 2026-08-08  
**Priority files:** `zone.c/h`, `common.c`, `vid_x.c`, `sys_linux.c`, `snd_linux.c`, `model.c`, `pr_exec.c`, `pr_edict.c`, `host.c` + skim `r_*.c`/`d_*.c`

---

## 1. Alloc / free API map

| API | File | Lifetime / semantics | Notes |
|-----|------|----------------------|-------|
| **`Memory_Init(buf, size)`** | `zone.c:913` | Owns entire contiguous hunk from `Sys_PageIn` / host | Sets `hunk_base`, `hunk_size`; carves zone + cache |
| **`Z_Malloc` / `Z_TagMalloc` / `Z_Free`** | `zone.c:99–211` | Small dynamic (strings, cvars, cmd argv) inside **zone** (~48KB default `DYNAMIC_SIZE` 0xc000) | Zero-filled; free merges adjacent free blocks; `Z_Free(NULL)` → `Sys_Error` |
| **`Hunk_Alloc` / `Hunk_AllocName`** | `zone.c:399–437` | Low stack; level/session data | 16-byte align; zero-filled; grows low; may purge cache via `Cache_FreeLow` |
| **`Hunk_HighAllocName`** | `zone.c:482` | High stack (video, temp) | Returns **NULL** on OOM (unlike low alloc which `Sys_Error`s) |
| **`Hunk_TempAlloc`** | `zone.c:528` | High, single-shot temp | Frees previous temp; sets `hunk_tempactive`; can return NULL |
| **`Hunk_FreeToLowMark` / `HighMark`** | `zone.c:444–474` | Bulk free by mark | Used by `Host_ClearMemory`, video resize |
| **`Cache_Alloc` / `Cache_Check` / `Cache_Free` / `Cache_Flush`** | `zone.c:824–903` | Demand-cached models/sounds between low/high | LRU purge; `Cache_Move` may free on failure |
| **`malloc` / `calloc` / `free` / `realloc`** | scattered | Outside hunk | `vid_x` framebuffers; `snd_linux` ring; `pr_knownstrings`; VCR argv; GL paths (N/A for x11 soft) |
| **X11 / SHM** | `vid_x.c` | `XCreateImage`+`malloc`, `XShmCreateImage`+`shmget`/`shmat` | Teardown incomplete / wrong free API |
| **ALSA** | `snd_linux.c` | `snd_pcm_*` + `calloc` ring | Fail path closes pcm + frees buffer |

**Memory layout (from `zone.h` comments):**  
`Zone → low hunk (host/client/server) → cache gap → high hunk (video/z/surfcache/temp)`

**Call-site patterns:**
- Files: `COM_LoadFile` → hunk / temp / zone / cache / stack (`common.c:1536`)
- Models: mostly `Hunk_AllocName` + alias `Cache_Alloc` (`model.c`)
- QC strings (LP64): negative `string_t` → `pr_knownstrings` + hunk (`pr_edict.c:1142+`)
- Sizebufs: data on hunk; `SZ_Free` only clears length (`common.c:744`)

---

## 2. Top 15 ranked findings

Severity: **S0** crash/exploit on normal play · **S1** crash on resize/map/mod · **S2** crash on malicious data · **S3** leak / hygiene

| # | Sev | Class | Location | Hypothesis |
|---|-----|-------|----------|------------|
| **1** | S0/S2 | **Buffer overflow** | `common.c:1172–1181` `va()` | `vsprintf` into static `string[1024]`. Used widely (paths, prints, errors). Long format → stack/static smash. Comment already says “FIXME: make this buffer size safe someday”. |
| **2** | S0/S2 | **Buffer overflow** | `common.c:914–981` `COM_Parse` | Quoted and unquoted tokens write `com_token[len++]` with **no** `len < sizeof(com_token)-1` check (`com_token[1024]`). Map entities / configs / QC-related text can overrun. |
| **3** | S1/S2 | **OOB write (QC VM)** | `pr_exec.c:546–559` `OP_STOREP_*` | `ptr = (eval_t *)((byte *)sv.edicts + b->_int)` — **no** range check vs `sv.num_edicts * pr_edict_size`. Bad field address → arbitrary write in edict arena / adjacent memory. |
| **4** | S1/S2 | **OOB read/write (QC VM)** | `pr_exec.c:561–593` `OP_ADDRESS` / `OP_LOAD_*` | Field index `b->_int` applied as `((int *)&ed->v + b->_int)` without clamp to `progs->entityfields`. `NUM_FOR_EDICT` only under `#ifdef PARANOID`. |
| **5** | S1/S2 | **OOB / unbounded PC** | `pr_exec.c:390–397`, `607–608` | Statement index `s` never checked against `progs->numstatements`. `OP_GOTO`/`OP_IF*` can jump anywhere; then `pr_statements[s]` + `pr_globals[st->a/b/c]` unbounded. |
| **6** | S1 | **Buffer overflow** | `host.c:90–97`, `121–135`; also `283+` | `Host_EndGame` / `Host_Error` / client print helpers: `vsprintf` into `string[1024]`. Same class as `va()`. |
| **7** | S1 | **Buffer overflow** | `pr_exec.c:261–267` `PR_RunError` | `vsprintf(string[1024], …)` on VM errors (can include names/offsets). |
| **8** | S1 | **Integer overflow → undersized alloc / OOB** | `vid_x.c:330–347`, `385–426` | `vid.width * vid.height * sizeof(*d_pzbuffer)` and `bytes_per_line * height` use `int`/`long` without checked mul. Full HD caps help (`MAXWIDTH` 1920 / `MAXHEIGHT` 1080 in `r_shared.h`) but resize `ConfigureNotify` (`vid_x.c:1012–1021`) can still feed large dims before clamp in some paths; `mem` for `malloc` similarly unchecked. |
| **9** | S1 | **Wrong free / UAF / double-free risk** | `vid_x.c:315–318`, `405–410` | Non-SHM: `free(x_image->data); free(x_image)` instead of `XDestroyImage`. SHM: `free(x_framebuffer[frm])` without `XDestroyImage`; `shmdt` after free of XImage shell. Resize path calls these repeatedly (`VID_Update` config notify). Classic heap corruption under X11. |
| **10** | S1 | **NULL deref** | `vid_x.c:349–360` | `malloc(mem)` result passed straight into `XCreateImage` **without NULL check**. OOM → crash inside Xlib or later on `->data`. |
| **11** | S1/S2 | **Buffer overflow** | `common.c:859–878` `COM_FileBase` | `strncpy(out, s2+1, s-s2)` into caller `base[32]` (`COM_LoadFile`) with **no cap at 31**. Long basenames smash stack. |
| **12** | S2 | **Buffer overflow / path** | `common.c:887–903` `COM_DefaultExtension`; `1289`, `1427`, `1440–1444`, `1714` | `strcat` / `sprintf` into `MAX_OSPATH` (128) without length checks (`COM_WriteFile`, cache paths, pak paths). |
| **13** | S1/S2 | **Integer overflow (zone)** | `zone.c:155–169` `Z_TagMalloc` | `size += sizeof(memblock_t) + 4` then align; large/negative `size` wraps → tiny block, later heap trash via sentinel write at `base+size-4`. `Hunk_AllocName` rejects `size < 0` but zone does not. |
| **14** | S1/S2 | **Missing bounds on lumps** | `model.c:355–399`, `504–546`, `1143+` | Lump loaders trust `fileofs`/`filelen`/`dataofs`/`width*height` without verifying range inside `com_filesize`. Malicious BSP → OOB read then `memcpy` into hunk; texture `pixels = w*h/64*85` can overflow. |
| **15** | S1 | **Heap OOB (VCR playback)** | `host.c:792–801` | `com_argv = malloc(com_argc * sizeof(char*))` then loop `com_argv[i+1] = p` for `i in [0, com_argc)` → writes **`com_argc+1` slots**. Off-by-one; also no NULL checks on `malloc`. |

### Honorable mentions (16–22)

| Class | Location | Note |
|-------|----------|------|
| **Underflow / OOB read** | `common.c:785–795` `SZ_Print` | If `cursize == 0`, reads `buf->data[-1]`. |
| **Buffer overflow** | `sys_linux.c:163` `Sys_Warn`; `250` `Sys_DebugLog` | Still `vsprintf` into 1024 (active `Sys_Printf` already fixed to `vsnprintf` 16384). |
| **Stack smash / cmd inject** | `sys_linux.c:261–276` `Sys_EditFile` | `sprintf(cmd[256], "xterm -e %s %s", editor, filename)` + `system()`. |
| **NULL / silent fail** | `zone.c:528–546` `Hunk_TempAlloc` | Propagates NULL from `Hunk_HighAllocName`; some callers check (`COM_LoadFile`), others may not. |
| **UAF-ish entity refs** | `pr_edict.c:133–137` `ED_Free` | Documented FIXME: does not scrub other edicts’ references to freed entity. |
| **PR_LoadProgs trust** | `pr_edict.c:1004–1092` | Offsets into `progs.dat` not validated against `com_filesize` before pointer arithmetic. |
| **Large stack** | `r_shared.h:65–69` + `r_main.c` | `NUMSTACKEDGES` 2400 / `NUMSTACKSURFACES` 800 on stack when under limits — large but fixed; watch with sanitizers. |
| **Leak** | `vid_x.c:747–752` `VID_Shutdown` | Closes display only; no SHM detach / image destroy / high-hunk video free. |
| **Leak** | `pr_edict.c:1044–1048` | `pr_knownstrings` freed on reload; engine string **contents** live on hunk until `Host_ClearMemory` — order-dependent. |
| **snd_linux** | `snd_linux.c` | Generally careful (NULL checks, fail path). Residual: `SNDDMA_GetDMAPos` uses `shm` without re-checking `shm->buffer` after delay math; low risk if shutdown races. |

---

## 3. Suggested first fixes (multi-agent order)

### Wave A — high ROI, low design risk (do first)

1. **`va()` → `vsnprintf(string, sizeof string, …)`** + ensure NUL; consider rotating multi-buffer if reentrancy matters (`common.c:1172`).
2. **`COM_Parse`:** clamp `len` to `sizeof(com_token)-1` for quoted + word paths (`common.c:943–981`).
3. **All remaining `vsprintf` in host/sys/pr paths** → `vsnprintf` with sized buffers (`host.c`, `pr_exec.c` `PR_RunError`, `sys_linux.c` `Sys_Warn`/`Sys_DebugLog`). Align with already-fixed `Sys_Printf` (`sys_linux.c:86–96`).
4. **`COM_FileBase`:** `Q_strncpy(out, …, outsize)` or hard cap 31; pass size from callers.
5. **Path `sprintf`/`strcat`:** `snprintf` into `MAX_OSPATH` (`COM_WriteFile`, `COM_DefaultExtension`, pak/cache paths).

### Wave B — X11 / video (resize is a real crash path)

6. **`ResetFrameBuffer` / `ResetSharedFrameBuffers`:**  
   - Use `XDestroyImage` (non-SHM) / proper SHM detach + `shmdt` + destroy image.  
   - NULL out slots after free.  
   - Check `malloc` before `XCreateImage`.  
   - Checked mul for `width*height*bpp` (reject / clamp before alloc).  
7. After every reset, set `vid.buffer` / `vid.conbuffer` from **`->data`**, not `XImage*` (footgun at `vid_x.c:362–363` even if `VID_Update` overwrites).
8. **`st2_fixup` / `st3_fixup`:** reject rects with `x+width > width` or `y+height > height` (st3 already has some guards; st2 weaker).

### Wave C — QC VM hardening (debug + PARANOID)

9. **`OP_STOREP` / `OP_LOAD` / `OP_ADDRESS`:** validate byte offset in `[0, sv.num_edicts * pr_edict_size - sizeof(eval)]` and field index `< entityfields` (always on in DEBUG builds).
10. **Statement PC bounds** after every branch; global operand index `< progs->numglobals`.
11. **`PR_LoadProgs`:** verify each lump offset+size ≤ `com_filesize` before use.
12. Enable **`PARANOID`** (and existing `Z_CheckHeap` / `Hunk_Check`) on `quake.x11-dbg`.

### Wave D — zone / models / host

13. **`Z_TagMalloc`:** reject `size < 0` or `size > zone->size`; use size_t / checked add for header+sentinel.
14. **`Mod_Load*`:** validate lump ranges; checked `width*height` for textures.
15. **VCR argv:** `malloc((com_argc + 1) * sizeof *com_argv)`; check NULL; fix loop indexing.
16. **`SZ_Print`:** if `cursize == 0`, treat as “no trailing 0” branch without reading `data[-1]`.

### Verification recipe (for implementers)

```text
make -f WinQuake/Makefile.linux quake.x11 DEBUG=1 SANITIZE=address,undefined
# smoke under Xmux:
./quake.x11-dbg -basedir . -width 800 -height 600 +map e1m1 +quit
# resize window; load several maps; optional timedemo
valgrind --track-origins=yes ./quake.x11-dbg -basedir . +map e1m1 +quit
```

---

## 4. Already improved (do not re-fix as new)

- `Sys_Printf` / `Sys_Error`: `vsnprintf` + large buffers (`sys_linux.c:86–154`).
- LP64 QC strings: `PR_GetString` / `PR_SetEngineString` / `EDICT_TO_PROG` byte offsets (`progs.h`, `pr_edict.c`).
- `PIXEL24` as `uint32_t` for 24/32 bpp fillup (`vid_x.c:110–116`) — prior SHM overrun class.
- `Q_strncpy` always NUL-terminates (`common.c:189–200`).
- `snd_linux.c` ALSA path: structured fail/cleanup, ring `calloc`.

---

## 5. Agent split recommendation

| Agent | Files | Focus |
|-------|-------|--------|
| **fix-printf** | `common.c`, `host.c`, `pr_exec.c`, `sys_linux.c` | Wave A string overflows |
| **fix-vid-x** | `vid_x.c` only | Wave B free/alloc/resize |
| **fix-qc-vm** | `pr_exec.c`, `pr_edict.c` | Wave C bounds |
| **fix-zone-model** | `zone.c`, `model.c`, `host.c` VCR | Wave D |
| **test-asan** | build + Xmux | Evidence logs under `/tmp` or `docs/evidence/` |

Do **not** parallel-edit the same file without worktree isolation.

---

## 6. Key line index (quick jump)

| Topic | Path:lines |
|-------|------------|
| Zone free/alloc | `zone.c:99–211` |
| Hunk alloc | `zone.c:399–546` |
| Cache | `zone.c:575–903` |
| `va` | `common.c:1172` |
| `COM_Parse` | `common.c:914` |
| `COM_LoadFile` | `common.c:1536` |
| `SZ_GetSpace`/`SZ_Print` | `common.c:757–795` |
| X11 reset/update | `vid_x.c:310–453`, `1006–1087` |
| QC execute | `pr_exec.c:361–666` |
| Edicts / strings | `pr_edict.c:104–150`, `1004–1216` |
| Host error / VCR | `host.c:90–145`, `792–801` |
| ALSA | `snd_linux.c:85–327` |
| BSP load | `model.c:355–399`, `1143–1180` |

---

*End of explore report. No code was modified.*
