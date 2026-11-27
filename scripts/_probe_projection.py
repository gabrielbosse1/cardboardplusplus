"""Throwaway probe: report model extents and render a grid of candidate projections so
the camera angle can be picked by eye instead of guessed. Not part of the build."""
import importlib.util
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

spec = importlib.util.spec_from_file_location("g", os.path.join(os.path.dirname(__file__), "gen_knuckles_svg.py"))
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)

md = os.path.join(g.RENDER_ROOT, g.MODEL_DIR["left"])
with open(os.path.join(md, g.MODEL_DIR["left"] + ".json"), encoding="utf-8") as fh:
    comps = json.load(fh)["components"]

raw, _ = g.load_obj(os.path.join(md, "body.obj"))
loc = comps["body"]["component_local"]
xf = raw @ g.euler_matrix(*loc["rotate_xyz"]).T + np.asarray(loc["origin"])
print("body raw    extents X %.4f Y %.4f Z %.4f" % (raw[:, 0].max()-raw[:, 0].min(), raw[:, 1].max()-raw[:, 1].min(), raw[:, 2].max()-raw[:, 2].min()))
print("body xform  extents X %.4f Y %.4f Z %.4f" % (xf[:, 0].max()-xf[:, 0].min(), xf[:, 1].max()-xf[:, 1].min(), xf[:, 2].max()-xf[:, 2].min()))
for k in ("trigger", "thumbstick", "trackpad", "button_a", "squeeze"):
    vv, _f = g.load_obj(os.path.join(md, comps[k]["filename"]))
    ll = comps[k]["component_local"]
    vv = vv @ g.euler_matrix(*ll["rotate_xyz"]).T + np.asarray(ll["origin"])
    print("%-10s centre X %+.4f Y %+.4f Z %+.4f" % (k, vv[:, 0].mean(), vv[:, 1].mean(), vv[:, 2].mean()))


def rot_x(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_z(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def tris(hand):
    out = []
    mdir = os.path.join(g.RENDER_ROOT, g.MODEL_DIR[hand])
    with open(os.path.join(mdir, g.MODEL_DIR[hand] + ".json"), encoding="utf-8") as fh:
        cc = json.load(fh)["components"]
    for name, comp in g.DRAWN:
        if comp not in cc:
            continue
        fn = cc[comp].get("filename")
        if not fn:
            continue
        v, f = g.load_obj(os.path.join(mdir, fn))
        if v.size == 0:
            continue
        l = cc[comp].get("component_local", {"origin": [0, 0, 0]})
        v = v @ g.euler_matrix(*l.get("rotate_xyz", [0, 0, 0])).T + np.asarray(l["origin"])
        for face in f:
            for i in range(1, len(face) - 1):
                out.append((v[[face[0], face[i], face[i + 1]]], name))
    return out


CANDIDATES = {
    "top XZ": lambda v: v[:, [0, 2]],
    "front XY": lambda v: v[:, [0, 1]],
    "side ZY": lambda v: v[:, [2, 1]],
    "iso 35": lambda v: (v @ rot_x(np.radians(55)).T @ rot_z(np.radians(-25)).T)[:, [0, 2]],
    "iso 60": lambda v: (v @ rot_x(np.radians(70)).T @ rot_z(np.radians(-20)).T)[:, [0, 2]],
    "iso 45 top": lambda v: (v @ rot_x(np.radians(45)).T @ rot_z(np.radians(0)).T)[:, [0, 1]],
}

T = tris("left")
S = 300
sheet = Image.new("RGB", (S * 3, S * 2), (12, 12, 14))
for idx, (label, fn) in enumerate(CANDIDATES.items()):
    tile = Image.new("RGB", (S, S), (12, 12, 14))
    draw = ImageDraw.Draw(tile)
    pts = np.vstack([fn(t) for t, _ in T])
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    span = (hi - lo).max()
    sc = (S - 20) / span
    ox = (S - (hi[0] - lo[0]) * sc) / 2
    oy = (S - (hi[1] - lo[1]) * sc) / 2
    for t, name in T:
        p = fn(t) * sc
        p = p - lo * sc + np.array([ox, oy])
        edge = name in ("trigger", "button_a", "thumbstick", "trackpad")
        fill = (10, 132, 255) if edge else (150, 150, 155)
        draw.polygon([tuple(p[0]), tuple(p[1]), tuple(p[2])], fill=fill)
    draw.text((6, 6), label, fill=(255, 255, 255))
    sheet.paste(tile, ((idx % 3) * S, (idx // 3) * S))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_proj_grid.png")
sheet.save(out)
print("grid:", out)