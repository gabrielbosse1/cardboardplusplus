"""Generate per-part PNG views of the Valve Index knuckles controller from
Valve's own shipped render-model geometry.

Source of truth is the local SteamVR install, not the web:
    SteamVR\\drivers\\indexcontroller\\resources\\rendermodels\\valve_controller_knu_1_0_{left,right}\\

SteamVR's own binding editor draws these same OBJs, split per component, which is
precisely why they can be highlighted one part at a time. Downloading a third-party
copy of the same model would add a licence question and nothing else, so this reads
the installed one.

    python scripts/gen_knuckles_svg.py [--preview]

Writes to bridge/crates/cardboard-bridge/ui/icons/:
    knu_{hand}.png           base view, flat fills and crease lines
    knu_{hand}_{part}_hl.png just that part in the accent colour, transparent behind

The highlight files are overlays rather than full re-renders, so the view layer can
stack one on the base instead of swapping a 150 KB image per hover.

--preview additionally writes PNG contact sheets next to the script, for eyeballing
the projection when the geometry or the light direction changes.

--------------------------------------------------------------------------------
HOW TO PLACE A PART CORRECTLY (read before touching load_model_transform)
--------------------------------------------------------------------------------

ONE transform, for every part. All of the component OBJs are authored in a single
assembled model frame: each part's raw vertices are an exact subset of Valve's own
whole-model OBJ (valve_controller_knu_1_0_{hand}.obj). Confirm it any time you doubt
it - the coincidence check is a nearest-neighbour distance from a part's raw verts to
the whole-model verts, and it reads 0.00000 for every part that ships.

So the placement of the whole controller is exactly one rigid transform, and it is the
body's component_local. That transform is what turns the model upright (the raw
whole-model OBJ lies on its side). Everything in DRAWN goes through it, in the same
frame, at the same time, with nothing else applied:

    verts = verts @ model_rotation.T + model_origin

Consequences worth knowing before changing anything:

  * There is no second transform. A part's own component_local is NOT its placement in
    this authoring frame and must not be applied on top of the model transform. Doing
    so puts every part in a different pose from the one it was authored in - they come
    out spun about their own axis and thrown outside the shell's silhouette, and the
    two buttons swap over relative to each other.

  * There are no nudges. No slides, no per-part offsets, no flips, no flattening, no
    principal-axis squaring to the camera. A part that looks out of place is a symptom
    of the wrong transform above, not of a missing correction. Adding an offset hides
    the symptom and leaves the pose wrong, which is what made the buttons drift to the
    opposite side of the controller.

  * No culling for the small parts. They are thin plates seen nearly edge-on, so
    back-face culling tears them into slivers. The shell is the only part culled. See
    build_part's `cull` argument, which load_hand fixes at name == "body".

The rest of the pipeline is what makes it read as line art rather than a shaded blob,
and each stage has its own comment where it runs: the edge-use silhouette and the
detail-line threshold in build_part, the lean and the per-hand mirror in apply_lean,
the union of both models' seams in unify_lines, the shared viewBox in view_box_for, and
the rasteriser in write_png.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

import numpy as np

# Logical part name -> render-model component key. The OBJ filename comes from that
# component's "filename" field rather than being guessed: Valve names them
# inconsistently (the `sys_button` component points at button_system.obj), and a
# hardcoded name breaks the moment one of them changes.
#
# `squeeze` is deliberately absent. It is the surface that moves as the grip closes, so
# its rest transform sits away from the shell and renders as a detached sliver. The
# Grip binding highlights `palm` (the shell) instead, which is what gripping means.
# `body` is the shell; the rest are the parts a binding can target.
DRAWN = [
    ("body", "body"),
    ("trackpad", "trackpad"),
    ("thumbstick", "thumbstick"),
    ("trigger", "trigger"),
    ("b", "button_b"),
    ("a", "button_a"),
    ("system", "sys_button"),
]

# Every OBJ is authored in one assembled frame: each part's own vertices are a subset of
# Valve's whole-model OBJ. So no part carries a transform of its own - they are all
# placed by the single rigid transform that turns the model upright, which is the body's
# component_local. See load_model_transform, and the module docstring above for why a
# per-part transform, a slide or any other nudge is the wrong tool here.

# Part that each GestureTarget highlights. Several targets share hardware (the two
# trackpad axes are one pad), and the two analog force targets share their button.
TARGET_PART = {
    0: "trigger",   # trigger
    1: "palm",      # grip
    2: "a",         # A
    3: "b",         # B
    4: "trackpad",  # trackpad
    5: "thumbstick",  # thumbstick
    6: "trackpad",  # trackpad X
    7: "trackpad",  # trackpad Y
    8: "thumbstick",  # thumbstick X
    9: "thumbstick",  # thumbstick Y
    10: "palm",     # grip force
    11: "trigger",  # trigger value
}

# SteamVR's binding editor draws these controllers as a flat line illustration, not as
# lit 3D: a dark interior under a pale outline. Lambert shading per triangle reads as an
# untextured grey blob at this size, so the two palettes below carry the whole look instead.
#
# FILL has to sit clearly above the panel background. Matched to it, the interior vanishes
# and the controller renders as a bare wireframe - the outline floats with no body under it.
FILL = np.array([0.21, 0.22, 0.26])
STROKE = np.array([0.86, 0.88, 0.92])   # line-art ink
# Interactive parts are outlined much fainter in the base view. At the same weight as the
# shell their creases read as stray lines lying on the controller - the trigger's paddle in
# particular looks like loose geometry rather than a part of the shell.
PART_STROKE = np.array([0.40, 0.43, 0.50])
# A highlight tints the part rather than replacing it, so its fill stays translucent and the
# base's own outline still reads through it. Opaque, the trigger - a broad paddle sitting
# mid-grip - looked like a sticker laid over the controller.
ACCENT = np.array([0.30, 0.72, 1.00])    # a highlighted part's outline
ACCENT_FILL = np.array([0.10, 0.42, 0.78])
ACCENT_FILL_ALPHA = 170

# Above this dihedral angle an interior edge is drawn as a detail line. The silhouette comes
# from edge-use counting and does not depend on this; a low value here is what turns the
# model back into a wireframe, so it stays high enough to keep only the disc rim and grip.
DETAIL_DEGREES = 38.0

# Both hands lean a few degrees away from the midline, the way the binding editor they
# are modelled on draws them. It is a rotation of the already-projected points, so it
# costs no second camera and adds no perspective - only the lean itself. The right hand's
# rotation is the left's reflected through the model's X axis, which is what keeps the
# pair an exact mirror instead of two hands that drift apart by the mesh differences.
LEAN_DEGREES = 18.0

# Turn about the vertical axis, per hand. Straight on, the trigger plate is edge-on and
# reads as a line; this brings its face around the way the editor these are modelled on
# draws it. Mirrored between the hands, for the same reason the lean is.
YAW_DEGREES = 28.0

STEAMVR = os.environ.get(
    "STEAMVR_ROOT",
    r"C:\Program Files (x86)\Steam\steamapps\common\SteamVR",
)
RENDER_ROOT = os.path.join(
    STEAMVR, "drivers", "indexcontroller", "resources", "rendermodels"
)
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "bridge", "crates", "cardboard-bridge", "ui", "icons")
MODEL_DIR = {"left": "valve_controller_knu_1_0_left", "right": "valve_controller_knu_1_0_right"}


def load_obj(path: str) -> tuple[np.ndarray, list[list[int]]]:
    """Reads vertices and faces, normalising face indices to 0-based.

    Valve's exporters emit 1-based indices with a `pos/uv/normal` triple. Negative
    indices (relative to the end) are accepted too, since that is legal OBJ and
    silently dropping those faces would leave holes in the silhouette."""
    verts: list[list[float]] = []
    faces: list[list[int]] = []
    with open(path, "r", encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            if line.startswith("v "):
                p = line.split()
                verts.append([float(p[1]), float(p[2]), float(p[3])])
            elif line.startswith("f "):
                idx = []
                for token in line.split()[1:]:
                    raw = int(token.split("/")[0])
                    idx.append(raw - 1 if raw > 0 else len(verts) + raw)
                if idx and all(0 <= i < len(verts) for i in idx):
                    faces.append(idx)
    return np.asarray(verts, dtype=np.float64), faces


def euler_matrix(rx: float, ry: float, rz: float) -> np.ndarray:
    """Render-model component_local rotation: XYZ degrees, applied in the runtime's order."""
    ax, ay, az = (math.radians(a) for a in (rx, ry, rz))
    cx, sx = math.cos(ax), math.sin(ax)
    cy, sy = math.cos(ay), math.sin(ay)
    rz_m = np.array([[math.cos(az), -math.sin(az), 0], [math.sin(az), math.cos(az), 0], [0, 0, 1]])
    rx_m = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    ry_m = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    return rz_m @ ry_m @ rx_m


def load_model_transform(components: dict):
    """The one rigid transform every part is drawn through.

    Valve's component_local on the body is the pose that turns the assembled model
    upright. Every OBJ is authored in that same assembled frame - a part's vertices are
    a subset of the whole-model OBJ - so applying this and nothing else puts each part
    exactly where it sits on the controller: the buttons on their recesses, in the order
    the model has them, with no nudge in the drawing needed."""
    local = components["body"].get("component_local", {"origin": [0, 0, 0]})
    rx, ry, rz = local.get("rotate_xyz", [0, 0, 0])
    return euler_matrix(rx, ry, rz), np.asarray(local["origin"], dtype=np.float64)


def yaw_matrix(deg: float) -> np.ndarray:
    """Turn about the model's up axis, which is what swings a side face into view."""
    ang = math.radians(deg)
    c, s = math.cos(ang), math.sin(ang)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def compose_yaw(model, model_dir: str, hand: str):
    """Adds the view yaw to the model transform.

    Seen straight on, the trigger plate is edge-on to the camera: its surface faces away
    from the viewer, so it reads as a thin line. Turning the model about its own long
    axis brings that surface around so it reads as a face, the way the binding editor
    these are modelled on draws it. The turn is per hand for the same reason the lean is
    - each controller's plate sits on its own outer side, so one yaw for both would turn
    one away from the camera and the other into it.

    The transform is applied as verts @ rot.T + origin, so the pair that comes back has
    to carry both the new rotation and the new origin, or the model swings off into
    space instead of turning in place."""
    rot, origin = model
    yaw_r = yaw_matrix(-YAW_DEGREES if hand == "left" else YAW_DEGREES)
    body_v, _f = load_obj(os.path.join(model_dir, "body.obj"))
    centre = (body_v @ rot.T + origin).mean(axis=0)
    return yaw_r @ rot, yaw_r @ (origin - centre) + centre


def build_part(components: dict, model_dir: str, component: str, cull: bool = True, model=None):
    """Loads one component and returns (projected_points, fills, creases).

    `fills` are the faces as flat-filled triangles; `creases` are the edges where two
    faces meet at a real angle. Splitting them is what produces the line-art look:
    filling every face flat hides the tessellation, and only creasing the edges that
    actually bend keeps the surface smooth instead of turning it into a wireframe.

    `cull` drops back-facing triangles. It is right for the shell, which is a closed
    volume with a visible front. Every other part is a thin plate - the plates read as
    dots and the trigger as a broad paddle - and a plate's silhouette is both of its
    surfaces together, so those opt out and fill every face. Culling a plate instead
    leaves a crescent that marks a sliver of the part.

    `model` is the one rigid transform the whole drawing goes through - see
    load_model_transform, and compose_yaw for the view turn that shows the trigger's
    face. Placing a part by anything else, its own component_local included, moves it
    out of the pose it was authored in.
    """
    filename = components[component].get("filename")
    if not filename:
        return np.zeros((0, 2)), [], []
    verts, faces = load_obj(os.path.join(model_dir, filename))
    if verts.size == 0:
        return np.zeros((0, 2)), [], []
    verts = verts @ model[0].T + model[1]
    pts = np.stack([verts[:, 0], -verts[:, 1]], axis=1)

    fills: list[str] = []
    # Edge -> the faces using it. An edge used once is a boundary of the surface; used
    # twice it is interior. That single count is most of the outline: a dihedral-angle
    # threshold on a mesh this dense draws thousands of tessellation edges and misses real
    # contours at the same time, which is why the result looked hand-drawn and incomplete.
    edge_faces: dict[tuple[int, int], int] = {}
    edge_points: dict[tuple[int, int], tuple[int, int]] = {}
    # Edge -> the two face normals, for the detail pass below.
    edge_normals: dict[tuple[int, int], list[np.ndarray]] = {}
    # Edge -> how many of its faces face the camera. Exactly one means the edge borders the
    # filled surface, which is the outline the eye reads; none means it belongs to the far
    # side, where only a real crease survives as interior line work.
    edge_front: dict[tuple[int, int], int] = {}
    for f in faces:
        for i in range(1, len(f) - 1):
            tri = [f[0], f[i], f[i + 1]]
            a, b, c = verts[tri[0]], verts[tri[1]], verts[tri[2]]
            n = np.cross(b - a, c - a)
            norm = np.linalg.norm(n)
            if norm < 1e-12:
                continue
            n = n / norm
            front = not cull or n[2] > 0.0
            if front:
                fills.append(" ".join(f"{pts[v][0]:.4f},{pts[v][1]:.4f}" for v in tri))
            # Edges are collected from every face, culled or not, so the creases that sit
            # on the far side of the shell are part of the line work as well - the outline
            # alone left the drawing looking like half a controller.
            for p, q in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
                key = (p, q) if p < q else (q, p)
                edge_faces[key] = edge_faces.get(key, 0) + 1
                edge_points.setdefault(key, (p, q))
                edge_normals.setdefault(key, []).append(n)
                if front:
                    edge_front[key] = edge_front.get(key, 0) + 1

    outlines: list[str] = []
    details: list[str] = []
    threshold = math.cos(math.radians(DETAIL_DEGREES))
    for key, count in edge_faces.items():
        if edge_front.get(key, 0) == 1:
            # Border of the filled surface: always part of the drawing.
            outlines.append(_segment(pts, *edge_points[key]))
            continue
        normals = edge_normals[key]
        if len(normals) == 2 and float(np.dot(normals[0], normals[1])) < threshold:
            # A real crease between two faces. At DETAIL_DEGREES this is a handful of lines
            # on the shell - the disc rim, the grip edges - rather than the whole mesh.
            details.append(_segment(pts, *edge_points[key]))
    return pts, fills, outlines + details


def _segment(pts: np.ndarray, p: int, q: int) -> str:
    return f"{pts[p][0]:.4f},{pts[p][1]:.4f} {pts[q][0]:.4f},{pts[q][1]:.4f}"


def load_hand(hand: str) -> list[tuple[str, np.ndarray, list, list]]:
    """Projected geometry for one hand, as (part_name, points, fills, creases)."""
    model_dir = os.path.join(RENDER_ROOT, MODEL_DIR[hand])
    with open(os.path.join(model_dir, f"{MODEL_DIR[hand]}.json"), "r", encoding="utf-8") as fh:
        components = json.load(fh)["components"]

    parts: list[tuple[str, np.ndarray, list, list]] = []
    model = compose_yaw(load_model_transform(components), model_dir, hand)
    for name, component in DRAWN:
        if component not in components:
            continue
        # The shell is the only part with a closed volume to hide behind; it draws just
        # its near surface. Every other part is a thin plate, and a plate's silhouette
        # is both of its surfaces together - culling one leaves a crescent that marks a
        # sliver of the part rather than the part.
        pts, fills, creases = build_part(
            components, model_dir, component, cull=name == "body", model=model
        )
        if pts.size == 0 or not fills:
            continue
        parts.append((name, pts, fills, creases))
    if not parts:
        raise SystemExit(f"no geometry found for {hand} in {model_dir}")
    return parts


def bbox_center(parts) -> tuple[float, float]:
    """Centre of everything a hand draws, in projected units - the lean pivots here."""
    allpts = np.vstack([p for _n, p, _f, _c in parts])
    lo, hi = allpts.min(axis=0), allpts.max(axis=0)
    return float((lo[0] + hi[0]) / 2), float((lo[1] + hi[1]) / 2)


def lean_points(xy: np.ndarray, hand: str, center: tuple[float, float]) -> np.ndarray:
    """Rotates projected points about a shared centre, mirroring the right hand back.

    The right hand reflects through X before and after the rotation, so both hands turn
    by the same matrix about their own mirrored centres instead of the two meshes
    rotating about different pivots and drifting apart."""
    out = xy.astype(np.float64).copy()
    mirror = hand == "right"
    if mirror:
        out[:, 0] = -out[:, 0]
    ang = math.radians(LEAN_DEGREES)
    ca, sa = math.cos(ang), math.sin(ang)
    dx, dy = out[:, 0] - center[0], out[:, 1] - center[1]
    out[:, 0] = center[0] + dx * ca - dy * sa
    out[:, 1] = center[1] + dx * sa + dy * ca
    if mirror:
        out[:, 0] = -out[:, 0]
    return out


def _mirror_x(coord: str) -> str:
    n = [float(v) for v in coord.replace(",", " ").split()]
    return " ".join(f"{-n[i]:.4f},{n[i + 1]:.4f}" for i in range(0, len(n), 2))


def _map_coord(coord: str, fn) -> str:
    n = [float(v) for v in coord.replace(",", " ").split()]
    pts = [fn(n[i], n[i + 1]) for i in range(0, len(n), 2)]
    return " ".join(f"{x:.4f},{y:.4f}" for x, y in pts)


def _lean_coord(coord: str, hand: str, center: tuple[float, float]) -> str:
    return _map_coord(coord, lambda x, y: tuple(lean_points(np.array([[x, y]]), hand, center)[0]))


def apply_lean(parts, hand: str, center: tuple[float, float]):
    """Leans every part of one hand: the vertex arrays the viewBox is measured from,
    and the fill and crease strings the rasteriser draws."""
    out = []
    for name, pts, fills, creases in parts:
        out.append(
            (
                name,
                lean_points(pts, hand, center),
                [_lean_coord(c, hand, center) for c in fills],
                [_lean_coord(c, hand, center) for c in creases],
            )
        )
    return out


def bbox(parts) -> tuple[np.ndarray, np.ndarray]:
    allpts = np.vstack([p for _n, p, _f, _c in parts])
    return allpts.min(axis=0), allpts.max(axis=0)


def recenter(parts, center: tuple[float, float]):
    """Slides one hand's whole drawing onto a shared centre.

    The shared viewBox has to cover both hands, and each hand only sits in its own half
    of it, so without this every image is two-thirds geometry and one-third empty margin
    and the controller draws small inside its box. Shifting each hand to the centre of
    that same box keeps one scale for both and doubles the size the drawing gets."""
    lo, hi = bbox(parts)
    mid = (lo + hi) / 2
    dx, dy = float(center[0] - mid[0]), float(center[1] - mid[1])
    return [
        (
            name,
            pts + np.array([dx, dy]),
            [_map_coord(f, lambda x, y: (x + dx, y + dy)) for f in fills],
            [_map_coord(c, lambda x, y: (x + dx, y + dy)) for c in creases],
        )
        for name, pts, fills, creases in parts
    ]


def unify_lines(left, right):
    """Makes both controllers draw the same detail lines.

    Valve exported the two models as separate meshes with different tessellation, so
    crease detection finds a different handful of edges on each and the pair ends up with
    lines one side is missing. Every seam found on either hand is drawn on both: the
    right hand's lines are reflected into the left hand's frame, the union is taken
    there, and the union is reflected back."""
    right_lines = {n: c for n, _p, _f, c in right}
    merged: dict[str, list[str]] = {}
    for name, _pts, _fills, creases in left:
        merged[name] = list(creases) + [_mirror_x(c) for c in right_lines.get(name, [])]
    left_out = [(n, p, f, merged[n]) for n, p, f, _c in left]
    right_out = [(n, p, f, [_mirror_x(c) for c in merged[n]]) for n, p, f, _c in right]
    return left_out, right_out


def view_box_for(hands: list[list[tuple[str, np.ndarray, list, list]]]) -> str:
    """One viewBox across both hands.

    The UI lays the two controllers side by side, so if each got its own viewBox the
    mirrored pair would render at different scales and sit at different heights. A
    shared box keeps them the same size and aligned without any layout maths."""
    allpts = np.vstack([p for hand in hands for _n, p, _f, _c in hand])
    lo, hi = allpts.min(axis=0), allpts.max(axis=0)
    pad = 0.006
    return f"{lo[0] - pad:.4f} {lo[1] - pad:.4f} {(hi[0] - lo[0]) + 2 * pad:.4f} {(hi[1] - lo[1]) + 2 * pad:.4f}", (
        float((lo[0] + hi[0]) / 2),
        float((lo[1] + hi[1]) / 2),
    )


def write_png(hand: str, parts, view_box: str, only: str | None, out_name: str) -> str:
    """Rasterises the projected geometry with Pillow.

    PNG rather than SVG: each part is a few thousand triangles, and Slint's SVG backend
    silently renders nothing at that path complexity (its own icons are hand-written
    handfuls of commands and do draw). Rasterising here also keeps the UI cheap - one
    small texture instead of a vector re-tessellation every frame.

    Drawn at SS times the display size and downsampled, which is what gives the edges
    their antialiasing.
    """
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        raise SystemExit("Pillow is required: pip install pillow")

    ss = 3
    target_w = 560  # per hand, roughly the size it renders at in the middle column
    vx, vy, vw, vh = [float(v) for v in view_box.split()]
    # Final size is the target; the canvas is that times ss, so downsampling is what
    # antialiases. Deriving the canvas from `vw * scale` instead would give target_w / ss
    # and leave the UI upscaling a third of the resolution it asked for.
    px_w = target_w
    px_h = max(1, int(round(target_w * vh / vw)))
    scale = px_w / vw

    img = Image.new("RGBA", (px_w * ss, px_h * ss), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    ox, oy = -vx * scale, -vy * scale
    highlight = only is not None
    line_w = max(1, ss)

    def to_px(coord: str, count: int) -> list[tuple[float, float]]:
        n = [float(v) for v in coord.replace(",", " ").split()]
        # Project, then scale world units to pixels, then shift by the viewBox origin.
        # ox/oy are already in pixels, so scale has to be applied to the raw coordinate
        # first - adding them to the raw value mixes units and collapses the geometry to
        # a single point.
        return [
            ((n[i] * scale + ox) * ss, (n[i + 1] * scale + oy) * ss)
            for i in range(0, count * 2, 2)
        ]

    def rgb(colour: np.ndarray, alpha: int = 255) -> tuple[int, int, int, int]:
        return tuple(int(round(float(c) * 255)) for c in colour) + (alpha,)

    for name, _pts, fills, creases in parts:
        if only is not None and name != only:
            continue
        for coord in fills:
            draw.polygon(
                to_px(coord, 3),
                fill=rgb(ACCENT_FILL, ACCENT_FILL_ALPHA) if highlight else rgb(FILL),
            )
    for name, _pts, fills, creases in parts:
        if only is not None and name != only:
            continue
        ink = ACCENT if highlight else (STROKE if name == "body" else PART_STROKE)
        for coord in creases:
            draw.line(to_px(coord, 2), fill=rgb(ink), width=line_w)

    img = img.resize((px_w, px_h), Image.LANCZOS)
    out = os.path.join(OUT_DIR, out_name)
    img.save(out, optimize=True)
    return out


def write_hand(hand: str, parts: list[tuple[str, np.ndarray, list, list]], view_box: str) -> int:
    """Base view plus one accent overlay per part a binding can target."""
    write_png(hand, parts, view_box, None, f"knu_{hand}.png")
    written = 1
    for name, _pts, _f, _c in parts:
        if name == "body":
            # Grip lights the whole shell, so it needs the body geometry in accent. It is
            # not a separate part, just a second rendering of the same triangles.
            write_png(hand, parts, view_box, "body", f"knu_{hand}_palm_hl.png")
            written += 1
            continue
        write_png(hand, parts, view_box, name, f"knu_{hand}_{name}_hl.png")
        written += 1
    return written


def _rgb(c: np.ndarray) -> tuple[int, int, int]:
    return tuple(int(round(float(v) * 255)) for v in c)


def preview(hands: dict[str, list[tuple[str, np.ndarray, list, list]]], cell: int = 300) -> None:
    """Contact sheet for eyeballing the drawing: the shell alone, then each part lit on top.

    Written next to this script rather than into ui/icons so iterating on the look does not
    mean rebuilding the bridge to see the result. Verification only - not used at runtime.
    """
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow missing, skipping preview", file=sys.stderr)
        return

    part_names = [n for n, _c in DRAWN if n != "body"]
    order = ["__base__"] + part_names
    hand = "left"
    parts = hands[hand]

    allpts = np.vstack([p for pn, p, _f, _l in parts])
    lo = (float(allpts[:, 0].min()), float(allpts[:, 1].min()))
    span = float(
        max(allpts[:, 0].max() - allpts[:, 0].min(), allpts[:, 1].max() - allpts[:, 1].min())
    ) * 1.06

    def draw_part(draw, name, fills, lines, fill_col, stroke_col, width):
        for coord in fills:
            n = [float(v) for v in coord.replace(",", " ").split()]
            draw.polygon(
                [
                    (
                        (n[i] - lo[0]) / span * (cell - 10) + 5,
                        (n[i + 1] - lo[1]) / span * (cell - 10) + 5,
                    )
                    for i in (0, 2, 4)
                ],
                fill=fill_col,
            )
        for coord in lines:
            n = [float(v) for v in coord.replace(",", " ").split()]
            draw.line(
                [
                    (
                        (n[i] - lo[0]) / span * (cell - 10) + 5,
                        (n[i + 1] - lo[1]) / span * (cell - 10) + 5,
                    )
                    for i in (0, 2)
                ],
                fill=stroke_col,
                width=width,
            )

    ss = 2
    W, H = cell * ss, cell * ss
    per_row = 4
    rows = (len(order) + per_row - 1) // per_row
    sheet = Image.new("RGB", (cell * per_row, (cell + 22) * rows), (12, 12, 14))
    dr_sheet = ImageDraw.Draw(sheet)

    for idx, which in enumerate(order):
        col, row = idx % per_row, idx // per_row
        tile = Image.new("RGB", (W, H), (12, 12, 14))
        draw = ImageDraw.Draw(tile)
        # Shell first, so a part that lands outside it is obvious.
        for name, _pts, fills, lines in parts:
            if name != "body":
                continue
            draw_part(draw, name, fills, lines, _rgb(FILL), _rgb(STROKE), 2 * ss)
        if which != "__base__":
            for name, _pts, fills, lines in parts:
                if name != which:
                    continue
                draw_part(
                    draw,
                    name,
                    fills,
                    lines,
                    _rgb(ACCENT_FILL),
                    _rgb(ACCENT),
                    2 * ss,
                )
        tile = tile.resize((cell, cell), Image.LANCZOS)
        sheet.paste(tile, (col * cell, row * (cell + 22)))
        dr_sheet.text((col * cell + 6, row * (cell + 22) + cell + 6), which, fill=(210, 210, 215))

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_preview_knu.png")
    sheet.save(out)
    print("preview:", out)

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true", help="also write a PNG contact sheet")
    args = ap.parse_args()

    if not os.path.isdir(RENDER_ROOT):
        print(f"SteamVR render models not found at {RENDER_ROOT}", file=sys.stderr)
        print("Set STEAMVR_ROOT to the SteamVR install directory.", file=sys.stderr)
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)

    left = load_hand("left")
    right = load_hand("right")
    center = bbox_center(left)
    left = apply_lean(left, "left", center)
    right = apply_lean(right, "right", center)
    left, right = unify_lines(left, right)
    # Each hand is drawn into its own image, so the only thing the shared box needs from
    # both is one scale and one alignment. Collapsing them onto the same centre first is
    # what makes that box the size of one controller instead of the two of them side by
    # side, which is what leaves a third of every image empty margin.
    left = recenter(left, (0.0, 0.0))
    right = recenter(right, (0.0, 0.0))
    hands = {"left": left, "right": right}
    box, _shared = view_box_for(list(hands.values()))
    count = 0
    for hand, parts in hands.items():
        count += write_hand(hand, parts, box)
        print(f"{hand}: {len(parts)} parts")
    if args.preview:
        preview(hands)
    print(f"wrote {count} PNGs to {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())