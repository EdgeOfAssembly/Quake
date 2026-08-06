# Quake Linux ports — TODO

Branch: `feature/linux-x11-gl`  
Last updated: 2026-08-06

## Done (this branch)

- [x] Modern `WinQuake/Makefile.linux` → `quake.x11` + `glquake` (x86_64)
- [x] Full HD limits (1920×1080), auto-detect resolution, larger hunk
- [x] LP64-clean QC strings (`PR_GetString` / engine string table, `q_stdint.h`)
- [x] X11 key auto-repeat fix + input pump before `Host_FilterTime`
- [x] ALSA sound backend (`snd_linux.c`, `-lasound`)
- [x] Keypad `K_KP_*` + GLX/X11 KeySym map
- [x] Experimental local VL play loop: `tools/quake_vl_loop.py` (Xmux + Ollama)

## High priority

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

## Later / eyecandy

- [ ] Dynamic lights from map light entities (torches, etc.) — needs asset/light source pass
- [ ] Vulkan path only if clearly faster than GLX on this machine
- [ ] Formal/`make verify` not applicable to full engine; keep `make test` smoke

## Run reminders

```bash
# Build
make -C WinQuake -f Makefile.linux -j$(nproc) install

# Play (repo root, id1 data)
./glquake -basedir . -window -width 1280 -height 720 +map e1m1

# Spectator
xmux start quake-gl --geometry 1280x800 --gl nvidia --no-attach
# SPECTATOR: xmux attach quake-gl --no-reconnect

# Experimental VL loop (slow; prefer future game model)
python3 tools/quake_vl_loop.py --session quake-gl --model qwen3-vl:4b --ticks 20 --bootstrap
```
