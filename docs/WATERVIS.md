# Water VIS (GL transparent water prep)

## Does this hurt software quake.x11?

**No.** Vis only changes which leaves are considered visible through water.
Software still draws **opaque** turb water; it does not use `r_wateralpha`.
Patched VIS is a no-op for software appearance and is safe to ship.

## What we prepared

| Item | Path |
|------|------|
| VisPatch tool | system `vispatch` 1.4.7; source `/tmp/vispatch-1.4.7` |
| id1 vis data | `tools/vispatch/id1.vis` (from SourceForge id1_vis.tgz) |
| Loose patched BSPs | `id1/maps/*.bsp` (`.bak` = pre-patch) |
| Override pak | `id1/pak2.pak` — maps only, searched **before** pak0/pak1 |

## Rebuild / re-patch

```bash
# extract maps from pak0 if needed, then:
cd id1/maps
vispatch "*.bsp" -data ../../tools/vispatch/id1.vis -new
# rebuild pak2
python3 tools/make_pak2_maps.py   # or the one-liner in repo history
```

## GL usage (later)

```bash
./glquake -basedir . +r_wateralpha 0.5 +map e1m1
```

Modern ports can also load external `maps/e1m1.vis` without patching; we used
classic VisPatch so stock engines that only read BSP VIS still work.
