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

## Baseline (2026-08-08, demo1, 640×480, -nosound)

| Build | FPS (3 runs) | Notes |
|-------|----------------|-------|
| Release pre-spans16 | ~361–366 | `D_DrawSpans8` only |
| Release **+ C D_DrawSpans16** | **~580–582** | **+60%** |
| PROFILE (-g, frame ptr) | ~611–615 | for `perf` |

Hot path before: `D_DrawSpans8` ~35% of cycles (perf).  
Water warp is a **feature**, not a bug.

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

Next candidates (if needed): `D_DrawZSpans`, `R_DrawSurfaceBlock8_mip0`, X11 blit — only if still Quake-side dominated.

### Iteration 2 (2026-08-08)

| Change | FPS | Keep? |
|--------|-----|-------|
| spans16 baseline | ~589 | yes |
| r_surf mip0/1 locals + colormap | noise | yes (clarity) |
| D_DrawZSpans pair unroll | noise | yes (clarity) |
| **D_DrawSpans32** (`d_subdiv16 2`) | **~604–623** | **yes optional** (~+3–5%) |
| default stays `d_subdiv16 1` (16px) for quality | | |

Use `+d_subdiv16 2` for max throughput timedemos.
