# qpak agent notes

- **Path in monorepo**: `quake/qpak/`
- **Compiler**: g++ only, `-std=gnu++23`
- **Test**: `make test` (bash contract tests; uses `/tmp/QUAKE/id1/PAK0.PAK` if present)
- **Docs**: `docs/PIPELINE.md` (extract → skin → Xmux Blender; orientation rules)
- **Durable**: commit/push RetroCodeMess; mirror `/mnt/RetroCodeMess` on this host
- **Do not** commit extracted Quake game assets (PAK contents, WAVs, MDLs from retail)
- **Screenshots**: after Blender import, `xmux screenshot` and **read the image** before claiming framing is good
