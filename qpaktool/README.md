# qpak

C++23 command-line tool for Quake **`.pak`** archives (`PACK` format), plus
helpers to turn alias **`.mdl`** monsters into OBJ+PNG and view them in Blender
(Xmux spectator / NVIDIA PRIME).

**Location in this repo:** `qpaktool/` (copied from `/mnt/qpaktool`).

## Build / test

```bash
cd qpaktool
make -s                # debug + ASan
make -s release
make -s test           # contract tests (+ real PAK0 if id1/ present)
make install           # optional → $(PREFIX)/bin/qpak
```

Requires **g++** (`-std=gnu++23`). No third-party C++ deps.

## CLI

No arguments → usage on stderr (exit 2).

```text
qpak list <pak> [--exact|--wildcard|--regex] [pattern]
qpak extract <pak> <outdir> [--exact|--wildcard|--regex] [pattern]
qpak -h | --help
qpak -v | --version
```

| Mode | Flag | Meaning |
|------|------|---------|
| (none) | — | List/extract **all** |
| exact | `--exact` | Full path equals pattern |
| wildcard | `--wildcard` / `--glob` | Shell `*` `?` (**default** if pattern given) |
| regex | `--regex` | ECMAScript search on full path |

```bash
qpak list id1/PAK0.PAK --wildcard 'progs/*.mdl'
qpak extract id1/PAK0.PAK out/ --wildcard '*ogre*'
```

Stdout = data (paths). Diagnostics on stderr.

## Docs

- **[docs/PIPELINE.md](docs/PIPELINE.md)** — PAK → MDL skin → Blender/Xmux, crotch-cam fix, ogre/shambler notes

## Scripts

| Script | Role |
|--------|------|
| `scripts/mdl_to_obj.py` | MDL + `palette.lmp` → OBJ + PNG skin |
| `scripts/blender_quake_monster.py` | Blender: stand, elevated cam, 360° spin (stock polys) |
| `scripts/blender_shambler_hipoly.py` | Subdiv L2 + maw densify + x4 skin + spin |

See **docs/PIPELINE.md** for Real-ESRGAN (`realesrgan-ncnn-vulkan`) and before/after poly counts.

## Durable copies (this host)

- Monorepo: `/tmp/RetroCodeMess` (tmpfs) → GitHub `EdgeOfAssembly/RetroCodeMess`
- Mirror: `/mnt/RetroCodeMess` when synced
- Standalone experiments may also live under `/mnt/qpaktool`

## Specs

- https://quakewiki.org/wiki/.pak  
- Unofficial Quake Specs §5 (Alias / IDPO models)
