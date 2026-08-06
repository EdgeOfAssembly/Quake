#!/usr/bin/env python3
"""Quake MDL viewer for Blender: upright + elevated 3/4 + continuous 360 spin.

Environment:
  QUAKE_OBJ       path to .obj (required)
  QUAKE_SKIN      path to skin .png (required)
  QUAKE_NAME      object name (default Monster)
  TURN_PERIOD_S   seconds per full revolution (default 18)

Designed for: xmux run <session> -- blender --factory-startup --python this_file
"""
from __future__ import annotations

import math
import os
import time
from pathlib import Path

import bpy
import mathutils

OBJ = Path(os.environ["QUAKE_OBJ"])
SKIN = Path(os.environ["QUAKE_SKIN"])
NAME = os.environ.get("QUAKE_NAME", "Monster")
TURN_PERIOD_S = float(os.environ.get("TURN_PERIOD_S", "18"))


def log(msg: str) -> None:
    print(f"quake-monster: {msg}", flush=True)


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
    fig.name = NAME
    log(f"{NAME} verts={len(fig.data.vertices)} polys={len(fig.data.polygons)}")

    img = bpy.data.images.load(str(SKIN))
    mat = bpy.data.materials.new(name=f"{NAME}Skin")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out_n = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfDiffuse")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.interpolation = "Closest"
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
    log(f"bbox X={sx:.2f} Y={sy:.2f} Z={sz:.2f}")

    # Quake alias models are Z-up. Only reorient if Z is clearly NOT height
    # (arms-out stand1 can make Y slightly larger — keep Z-up then).
    max_dim = max(sx, sy, sz)
    if sz >= max_dim * 0.85:
        log("stand: keep Quake Z-up")
    elif sy >= max_dim * 0.85:
        fig.rotation_euler[0] = math.radians(-90.0)
        log("stand: X-90 (Y was clearly tallest)")
        bpy.ops.object.transform_apply(rotation=True)
    elif sx >= max_dim * 0.85:
        fig.rotation_euler[1] = math.radians(90.0)
        log("stand: Y90 (X was clearly tallest)")
        bpy.ops.object.transform_apply(rotation=True)

    coords = [fig.matrix_world @ v.co for v in fig.data.vertices]
    cx = sum(c.x for c in coords) / len(coords)
    cy = sum(c.y for c in coords) / len(coords)
    min_z = min(c.z for c in coords)
    max_z = max(c.z for c in coords)
    fig.location.x -= cx
    fig.location.y -= cy
    fig.location.z -= min_z
    bpy.ops.object.transform_apply(location=True)
    log(f"height={max_z - min_z:.2f}")

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
                    # Chest aim — not bbox center (crotch-cam)
                    center = mathutils.Vector(
                        (center.x, center.y, min_z + (max_z - min_z) * 0.55)
                    )
                    radius = max((c - center).length for c in coords)
                    elev = math.radians(18.0)
                    dist = radius * 2.8
                    eye = mathutils.Vector(
                        (
                            center.x + dist * math.sin(math.radians(15.0)),
                            center.y - dist * math.cos(math.radians(15.0)),
                            center.z + dist * math.sin(elev) * 0.85 + radius * 0.15,
                        )
                    )
                    direction = center - eye
                    rv3d = space.region_3d
                    rv3d.view_perspective = "PERSP"
                    rv3d.view_rotation = direction.to_track_quat("-Z", "Y")
                    rv3d.view_location = center
                    rv3d.view_distance = dist
                    log(f"cam z={center.z:.2f} dist={dist:.2f}")
            break

    state = {"t0": time.time(), "name": fig.name, "base_yaw": base_yaw}

    def spin():
        if state["name"] not in bpy.data.objects:
            return None
        obj = bpy.data.objects[state["name"]]
        elapsed = time.time() - state["t0"]
        obj.rotation_euler[0] = 0.0
        obj.rotation_euler[1] = 0.0
        obj.rotation_euler[2] = state["base_yaw"] + (elapsed / TURN_PERIOD_S) * math.tau
        try:
            bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
        except Exception:
            pass
        return 1.0 / 30.0

    bpy.app.timers.register(spin, first_interval=0.05, persistent=True)
    log(f"spin {TURN_PERIOD_S}s/rev")


if __name__ == "__main__":
    main()
