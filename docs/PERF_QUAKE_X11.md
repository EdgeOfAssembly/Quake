# quake.x11 performance profiling

## Commands

```bash
# From repo root (needs DISPLAY / Xmux)
make profile          # PROFILE build + timedemo ×3 → docs/evidence/perf/baseline.txt
make profile-perf     # + perf record/report
make profile-gprof    # gprof (-pg) variant
```

**Stable bench args:** `-window -width 640 -height 480 -nosound -profile +timedemo demo1`  
`-profile` exits after timedemo (see `CL_FinishTimeDemo`).  
`-nosound` avoids ALSA futex stalls under automated load.

**Spectator:** `xmux start quake-perf --geometry 800x600 --gl mesa --no-attach`  
`SPECTATOR: xmux attach quake-perf --no-reconnect`

## Baseline (demo1, 640×480, -nosound)

| Build | FPS | Notes |
|-------|-----|-------|
| Release pre-spans16 (2026-08-08) | ~361–366 | `D_DrawSpans8` only |
| Release + C D_DrawSpans16 | ~580–582 | +60% |
| PROFILE after spans16 | ~611–615 | for `perf` |
| **PROFILE 32bpp pre-opt (2026-08-09)** | **~530–551** | heap OOM fixed; truecolor path |
| **PROFILE after iter 3** | **~670–680** median | see log below |
| **Release after iter 3** | **~677** | stable ×3 |

Hot path now (32bpp): `D_DrawSpans32bpp` ~26%, `R_DrawSurfaceBlock32` ~9–11%, `D_DrawZSpans` ~8–9%.  
Water warp is a **feature**, not a bug.

**Heap:** default SW `-mem` is **384 MiB** (surfcache floor scales with res; was 128 MiB fixed and broke 640×480 profile).

## Iteration rule

1. `make profile` / timedemo → record FPS  
2. Change one thing  
3. Re-timedemo; if FPS drops → revert  
4. Stop when top Quake symbols are small or X11/kernel dominated  

## Iteration log

| Change | FPS (640×480 -nosound demo1) | Keep? |
|--------|------------------------------|-------|
| Baseline D_DrawSpans8 | ~363 | — |
| C D_DrawSpans16 (d_subdiv16 default 1) | **~581** | **YES +60%** |
| Inner unroll×4 on spans16 | ~570 | **REVERT** (slower) |

### Iteration 2 (2026-08-08)

| Change | FPS | Keep? |
|--------|-----|-------|
| spans16 baseline | ~589 | yes |
| r_surf mip0/1 locals + colormap | noise | yes (clarity) |
| D_DrawZSpans pair unroll | noise | yes (clarity) |
| **D_DrawSpans32** (`d_subdiv16 2`) | **~604–623** | **yes optional** (~+3–5%) |
| default stays `d_subdiv16 1` (16px) for quality | | |

### Iteration 3 (2026-08-09) — 32bpp / HUD / cache

| Change | PROFILE FPS | Keep? |
|--------|-------------|-------|
| Pre-opt 32bpp (heap fix only) | ~540 | baseline |
| Draw_PicFit fixed-point + clip; R_DrawRect32; spans32bpp 32px+unroll×4 | **~608** | **YES +12%** |
| Pre-expand backtile → memcpy tile clear | **~649** | **YES +7%** |
| PoT cachewidth shift; lightrow; 64px subdiv; zspan/surf/polyse polish | **~670–680** | **YES ~+4%** |
| Further micro-opts | noise | stop |

**Stop reason:** top symbol still `D_DrawSpans32bpp` (~26%); further gains need SIMD/ASM, not more C micro-opts. No FPS regression kept.
