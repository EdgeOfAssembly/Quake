# Quake Linux ports — TODO

Branch: `feature/dynlights` (dynlights) / `feature/linux-x11-gl` (base ports)  
Last updated: 2026-08-06

## Done

- [x] Modern `WinQuake/Makefile.linux` → `quake.x11` + `glquake` (x86_64)
- [x] Full HD limits (1920×1080), auto-detect resolution, larger hunk
- [x] LP64-clean QC strings (`PR_GetString` / engine string table, `q_stdint.h`)
- [x] X11 key auto-repeat fix + input pump before `Host_FilterTime`
- [x] ALSA sound backend (`snd_linux.c`, `-lasound`)
- [x] Keypad `K_KP_*` + GLX/X11 KeySym map
- [x] Experimental local VL play loop: `tools/quake_vl_loop.py` (Xmux + Ollama)
- [x] Light catalogue: `docs/LIGHT_SOURCES.md`, e1m1 `tlight*` PNGs / face list
- [x] **Dynamic lights v1 (GL)** — `feature/dynlights`: `tlight*` face probes, noflash surface pools, quadratic falloff (`gl_dynlights.c`); FIXUP removed onion-ring flashblend on map lamps

## Next priority — lighting quality (do these next)

### 1. Disable baked / sector lightmaps (see dyn sources clearly)

Goal: start of **e1m1** (and generally) should **not** use precomputed lightmaps so only auto dynlights show what fixtures illuminate.

- [x] **`r_dynlights_only 1`**: skip baked lightmaps in `R_BuildLightMap` (dyn probes only)
- [x] **`r_dynlights_ambient`**: ambient floor when only-mode (default 8)
- [x] Toggle rebuilds lightmaps immediately (`R_DynLightsForceLightmapRebuild`)
- [ ] Screenshot proof under Xmux: dark room except pools under `tlight*` / torches
- [ ] Optional: default only-mode on e1m1 for lighting debug

### 2. Higher-quality light rays / falloff

Current: mono lightmap luxels (16-unit) + quadratic dlight — better than flashblend rings, still blocky.

- [ ] Higher-res or filtered lightmap sampling (reduce luxel banding on walls)
- [ ] **RGB lightmaps** or colored additive surface pass (true warm yellow from `tlight*`, not luminance-only)
- [ ] Smoother falloff (inverse-square / multi-tap) and optional soft corona without onion rings
- [ ] Prefer face probes only; keep `r_dynlights_entities 0|1|2` as now
- [ ] Optional later: small GL shader or light volumes — still GL first (GTX 1050); CUDA bake offline only if useful

See `docs/LIGHT_SOURCES.md` (e1m1 ceilings, `sky4`, `tlight*` table).

## High priority (other)

### Game model (weekend) — **planned**

Generic `qwen3-vl:4b` is too slow (~70s/tick on GTX 1050) and hallucinates enemies/menus.
Train or fine-tune a **Quake-specific game model / policy** instead of raw general VL.

- [ ] **Dataset:** log human (or godmode) play: frames and/or engine state → actions
- [ ] Prefer **engine STATUS** (pos, angles, HP, armor, ammo, nearby ents) over pixels-only
- [ ] **Small real-time policy** (state → keys) for combat latency; optional tiny VL for scene ID
- [ ] Export/train path: GGUF or local trainer; run via CUDA **llama.cpp** (not Ollama in the hot loop)
- [ ] VRAM rule: GTX 1050 4GB — **do not co-run Ollama + llama-server**
- [ ] Eval: Xmux spectator session, kill ≥1 grunt on e1m1 without god if possible

Related notes: `~/.grok/memory/projects/quake-linux.md`  
Prototype loop (not production): `tools/quake_vl_loop.py`

### Engine / ports — **quake.x11 debug & memory (active goal)**

Full brief: **`docs/GOAL_QUAKE_X11_DEBUG_MEMORY.md`**

- [ ] Debug build: asserts on, `-O0 -g3`, `quake.x11-dbg`
- [ ] ASan + UBSan (+ Valgrind / GDB recipes)
- [ ] Multi-agent hunt: uninit, NULL, UAF, double-free, buffer over/underflow, stack smash, leaks
- [ ] `stdint` / `q_stdint.h` hygiene; widen on clear overflow risk
- [ ] Loop policy: `size_t` / `ssize_t`, `const size_t` length outside loop, hoist invariants

### Engine / ports

- [ ] Software `quake.x11` interactive proof (FHD) under Xmux
- [ ] MSAA / anisotropy / particle polish for `glquake`
- [ ] Optional SDL2 input path (Windows DirectInput replacement research)
- [ ] CD audio / music (CDAUDIO open failed without `/dev/cdrom`)
- [ ] Merge/cleanup: apply remaining useful bits from `patches/` as needed

### Agent play infrastructure

- [ ] Heartbeat motor loop (move every ~0.5s) + rare vision/policy ticks
- [ ] Per-tick frame dump for audit (`tick_NNN.jpg`) — catch VL lies
- [ ] Stricter “enemy/menu” detection (no free-text hallucination boost)
- [ ] Monitor-based live tick follow (not sleep-poll)

## Tools in-repo

- [x] **`qpaktool/`** — Quake `.pak` CLI + MDL→OBJ/Blender helpers (from `/mnt/qpaktool`)
  - Build: `make -C qpaktool -s test`
  - Useful for asset extract before game-model / lighting work

## Later / eyecandy

- [ ] Vulkan path only if clearly faster than GLX on this machine
- [ ] Formal/`make verify` not applicable to full engine; keep `make test` smoke

## Run reminders

```bash
# Build
make -C WinQuake -f Makefile.linux -j$(nproc) install

# Play dynlights (feature/dynlights)
./glquake -basedir . -window -width 1280 -height 720 +map e1m1 +r_dynlights 1
# r_dynlights_entities 0 = tlight faces only; 1 = +torch/flame; 2 = +all bake lights

# Spectator
xmux start quake-gl --geometry 1280x800 --gl nvidia --no-attach
# SPECTATOR: xmux attach quake-gl --no-reconnect

# Experimental VL loop (slow; prefer future game model)
python3 tools/quake_vl_loop.py --session quake-gl --model qwen3-vl:4b --ticks 20 --bootstrap
```
