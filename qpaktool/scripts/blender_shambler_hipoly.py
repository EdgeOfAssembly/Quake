#!/usr/bin/env python3
"""Shambler remaster viewer: subdiv L2 + maw densify + optional x4 skin + 360 spin.

Expects pre-converted assets (see docs/PIPELINE.md):
  /tmp/quake-shambler-blender/shambler.obj
  /tmp/quake-shambler-blender/shambler_skin_x4.png   # or shambler_skin.png

Environment overrides:
  QUAKE_OBJ, QUAKE_SKIN, TURN_PERIOD_S
"""
from __future__ import annotations

import math
import os
import time
from pathlib import Path

import bmesh
import bpy
import mathutils

OBJ = Path(os.environ.get("QUAKE_OBJ", "/tmp/quake-shambler-blender/shambler.obj"))
SKIN = Path(
    os.environ.get(
        "QUAKE_SKIN",
        "/tmp/quake-shambler-blender/shambler_skin_x4.png",
    )
)
TURN_PERIOD_S = float(os.environ.get("TURN_PERIOD_S", "18"))


def log(msg: str) -> None:
    print(f"shambler-hipoly: {msg}", flush=True)


def mesh_stats(obj: bpy.types.Object) -> str:
    return (
        f"verts={len(obj.data.vertices)} "
        f"edges={len(obj.data.edges)} "
        f"polys={len(obj.data.polygons)}"
    )


def main() -> None:
    try:
        bpy.context.preferences.view.show_splash = False
    except Exception:
        pass

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    try:
        bpy.ops.wm.obj_import(filepath=str(OBJ))
    except Exception:
        bpy.ops.import_scene.obj(filepath=str(OBJ))

    fig = next(o for o in bpy.context.scene.objects if o.type == "MESH")
    fig.name = "Shambler"
    log(f"BEFORE_SUBDIV {mesh_stats(fig)}")

    if not SKIN.is_file():
        raise SystemExit(f"missing skin: {SKIN}")
    img = bpy.data.images.load(str(SKIN))
    mat = bpy.data.materials.new(name="ShamblerSkin")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out_n = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfDiffuse")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.interpolation = "Linear"
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Color"])
    nt.links.new(bsdf.outputs["BSDF"], out_n.inputs["Surface"])
    fig.data.materials.clear()
    fig.data.materials.append(mat)

    bpy.ops.object.select_all(action="DESELECT")
    fig.select_set(True)
    bpy.context.view_layer.objects.active = fig
    fig.location = (0.0, 0.0, 0.0)
    fig.rotation_euler = (0.0, 0.0, 0.0)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    coords = [fig.matrix_world @ v.co for v in fig.data.vertices]
    xs = [c.x for c in coords]
    ys = [c.y for c in coords]
    zs = [c.z for c in coords]
    sx, sy, sz = max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)
    max_dim = max(sx, sy, sz)
    if sz < max_dim * 0.85:
        if sy >= max_dim * 0.85:
            fig.rotation_euler[0] = math.radians(-90.0)
            bpy.ops.object.transform_apply(rotation=True)
        elif sx >= max_dim * 0.85:
            fig.rotation_euler[1] = math.radians(90.0)
            bpy.ops.object.transform_apply(rotation=True)

    coords = [fig.matrix_world @ v.co for v in fig.data.vertices]
    cx = sum(c.x for c in coords) / len(coords)
    cy = sum(c.y for c in coords) / len(coords)
    min_z = min(c.z for c in coords)
    fig.location.x -= cx
    fig.location.y -= cy
    fig.location.z -= min_z
    bpy.ops.object.transform_apply(location=True)

    mod = fig.modifiers.new(name="Subdiv", type="SUBSURF")
    mod.levels = 2
    mod.render_levels = 2
    bpy.ops.object.modifier_apply(modifier="Subdiv")
    log(f"AFTER_SUBDIV_L2 {mesh_stats(fig)}")

    bpy.ops.object.mode_set(mode="EDIT")
    bm = bmesh.from_edit_mesh(fig.data)
    bm.verts.ensure_lookup_table()
    zs = [v.co.z for v in bm.verts]
    xs = [v.co.x for v in bm.verts]
    z_lo, z_hi = min(zs), max(zs)
    x_span = max(xs) - min(xs)
    head_verts = []
    for v in bm.verts:
        z_norm = (v.co.z - z_lo) / max(z_hi - z_lo, 1e-6)
        if z_norm >= 0.58 and abs(v.co.x) < x_span * 0.5:
            head_verts.append(v)
        elif z_norm >= 0.68:
            head_verts.append(v)
    log(f"maw/head verts for subdiv={len(head_verts)}")
    if head_verts:
        edges = set()
        for v in head_verts:
            for e in v.link_edges:
                edges.add(e)
        bmesh.ops.subdivide_edges(
            bm,
            edges=list(edges),
            cuts=1,
            use_grid_fill=True,
        )
        for _ in range(2):
            bmesh.ops.smooth_vert(
                bm,
                verts=[
                    v
                    for v in bm.verts
                    if (v.co.z - z_lo) / max(z_hi - z_lo, 1e-6) >= 0.55
                ],
                factor=0.4,
            )
        bmesh.update_edit_mesh(fig.data)
    bpy.ops.object.mode_set(mode="OBJECT")
    log(f"AFTER_MAW_SUBDIV {mesh_stats(fig)}")

    bpy.ops.object.shade_smooth()
    try:
        fig.data.use_auto_smooth = True
        fig.data.auto_smooth_angle = math.radians(30.0)
    except Exception:
        pass

    base_yaw = math.radians(25.0)
    fig.rotation_euler[2] = base_yaw

    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != "VIEW_3D":
                continue
            for space in area.spaces:
                if space.type != "VIEW_3D":
                    continue
                space.shading.type = "SOLID"
                try:
                    space.shading.color_type = "TEXTURE"
                    space.shading.light = "STUDIO"
                except Exception:
                    pass
                space.overlay.show_overlays = False
                region = next(r for r in area.regions if r.type == "WINDOW")
                with bpy.context.temp_override(window=window, area=area, region=region):
                    try:
                        bpy.ops.view3d.view_selected(use_all_regions=False)
                    except Exception:
                        pass
                    coords = [fig.matrix_world @ v.co for v in fig.data.vertices]
                    center = sum(coords, mathutils.Vector((0, 0, 0))) / len(coords)
                    min_z = min(c.z for c in coords)
                    max_z = max(c.z for c in coords)
                    center = mathutils.Vector(
                        (center.x, center.y, min_z + (max_z - min_z) * 0.65)
                    )
                    radius = max((c - center).length for c in coords)
                    elev = math.radians(14.0)
                    dist = radius * 2.3
                    eye = mathutils.Vector(
                        (
                            center.x + dist * math.sin(math.radians(20.0)),
                            center.y - dist * math.cos(math.radians(20.0)),
                            center.z + dist * math.sin(elev) * 0.85 + radius * 0.08,
                        )
                    )
                    direction = center - eye
                    rv3d = space.region_3d
                    rv3d.view_perspective = "PERSP"
                    rv3d.view_rotation = direction.to_track_quat("-Z", "Y")
                    rv3d.view_location = center
                    rv3d.view_distance = dist
            break

    state = {"t0": time.time(), "name": fig.name, "base_yaw": base_yaw}

    def spin() -> float | None:
        if state["name"] not in bpy.data.objects:
            return None
        obj = bpy.data.objects[state["name"]]
        elapsed = time.time() - state["t0"]
        obj.rotation_euler[2] = state["base_yaw"] + (elapsed / TURN_PERIOD_S) * math.tau
        try:
            bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
        except Exception:
            pass
        return 1.0 / 30.0

    bpy.app.timers.register(spin, first_interval=0.05, persistent=True)
    log("ready hipoly spin")


if __name__ == "__main__":
    main()
