"""Generate the bare-hand full-fist grip limit for vr_glove.

Reads Valve's bind tables straight out of the Rust source (no transcription),
curls each finger about its own hinge axis, runs the same parent-relative
transform composition the live skeleton path uses, and emits a C++ table of 31
parent-space VRBoneTransform-equivalents per hand.
"""
import math
import re
import sys

RUST = sys.argv[1]
OUT_C = sys.argv[2]

src = open(RUST, encoding="utf-8").read()


def table(name):
    m = re.search(r"const %s: \[\[f32; 3\]; NUM_BONES\] = \[(.*?)\n\];" % name, src, re.S)
    body = m.group(1)
    rows = re.findall(r"\[([^\]]*)\]", body)
    return [[float(x) for x in r.split(",")] for r in rows]


def quats(name):
    m = re.search(r"const %s: \[\[f32; 4\]; NUM_BONES\] = \[(.*?)\n\];" % name, src, re.S)
    body = m.group(1)
    rows = re.findall(r"\[([^\]]*)\]", body)
    return [[float(x) for x in r.split(",")] for r in rows]


POS_L, POS_R = table("BIND_POS_LEFT"), table("BIND_POS_RIGHT")
QUAT_L, QUAT_R = quats("BIND_QUAT_LEFT"), quats("BIND_QUAT_RIGHT")
assert len(POS_L) == len(POS_R) == len(QUAT_L) == len(QUAT_R) == 31, "table size"

PARENT = [-1, 0, 1, 2, 3, 4,
          1, 6, 7, 8, 9,
          1, 11, 12, 13, 14,
          1, 16, 17, 18, 19,
          1, 21, 22, 23, 24,
          0, 0, 0, 0, 0]

ORIENT_SEG = {
    2: (2, 3), 3: (3, 4), 4: (4, 5),
    6: (6, 7), 7: (7, 8), 8: (8, 9), 9: (9, 10),
    11: (11, 12), 12: (12, 13), 13: (13, 14), 14: (14, 15),
    16: (16, 17), 17: (17, 18), 18: (18, 19), 19: (19, 20),
    21: (21, 22), 22: (22, 23), 23: (23, 24), 24: (24, 25),
}
AUX_TIP = {26: 5, 27: 10, 28: 15, 29: 20, 30: 25}


def flexion(pos, bone):
    """Angle at `bone` between its own outgoing segment and its incoming one."""
    _, child = ORIENT_SEG[bone]
    parent = PARENT[bone]
    outgoing = unit(sub(pos[child], pos[bone]))
    incoming = unit(sub(pos[bone], pos[parent]))
    return math.degrees(math.acos(max(-1.0, min(1.0, dot(outgoing, incoming)))))


# Metacarpal bone, phalangeal chain, and the ABSOLUTE flexion each joint should
# end at in a closed fist (MCP, PIP, DIP), in degrees. Valve's bind pose is not
# flat — it already carries 13-29 deg of flexion — so these are targets, not
# amounts to add. Human maxima: MCP ~90, PIP ~110, DIP ~80.
FINGERS = [
    (6, [7, 8, 9], [85.0, 100.0, 65.0]),
    (11, [12, 13, 14], [90.0, 105.0, 70.0]),
    (16, [17, 18, 19], [90.0, 105.0, 70.0]),
    (21, [22, 23, 24], [80.0, 100.0, 65.0]),
]
THUMB_CHAIN, THUMB_TARGET = [3, 4], [50.0, 45.0]


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def norm(v):
    return math.sqrt(dot(v, v))


def unit(v):
    l = norm(v)
    return [x / l for x in v] if l > 1e-12 else [0.0, 0.0, 0.0]


def palm_normal(p):
    ring = [p[0], p[6], p[11], p[16], p[21]]   # wrist + the four metacarpal bones
    n = [0.0, 0.0, 0.0]
    for i in range(5):
        a, b = ring[i], ring[(i + 1) % 5]
        n[0] += a[1] * b[2] - a[2] * b[1]
        n[1] += a[2] * b[0] - a[0] * b[2]
        n[2] += a[0] * b[1] - a[1] * b[0]
    return unit(n)


def rotate_point(p, origin, axis, ang):
    """Rodrigues rotation of p about the line through origin along unit axis."""
    v = sub(p, origin)
    c, s = math.cos(ang), math.sin(ang)
    cr = cross(axis, v)
    dt = dot(axis, v)
    return [origin[i] + v[i] * c + cr[i] * s + axis[i] * dt * (1.0 - c) for i in range(3)]


def curl(pos, chain, target_degs, palm):
    """Rotate each bone's distal subtree until that joint hits its target flexion.

    The axis is perpendicular to the incoming bone and the palm normal, so the
    joint hinges rather than twists. Which of the two directions along that axis
    closes the joint is probed rather than assumed: the two bind tables are only
    partly mirrored (the finger columns are not exact x-reflections of each
    other), so a hardcoded sign folds one hand into a fist and the other inside
    out.
    """
    out = [list(p) for p in pos]
    for bone, target in zip(chain, target_degs):
        parent = PARENT[bone]
        if parent < 0:
            continue
        kids = [b for b in range(31) if is_descendant(b, bone)]
        if not kids:
            continue
        axis = unit(cross(palm, sub(out[bone], out[parent])))
        if norm(axis) < 0.5:
            continue
        # Pick the rotation sense that increases flexion at this joint.
        probe = math.radians(1.0)
        trial = [list(p) for p in out]
        for k in kids:
            trial[k] = rotate_point(out[k], out[bone], axis, probe)
        sign = 1.0 if flexion(trial, bone) > flexion(out, bone) else -1.0
        delta = target - flexion(out, bone)
        a = sign * math.radians(delta)
        for k in kids:
            out[k] = rotate_point(out[k], out[bone], axis, a)
    return out


def is_descendant(b, ancestor):
    p = PARENT[b]
    while p >= 0:
        if p == ancestor:
            return True
        p = PARENT[p]
    return False


# Quaternion helpers (w, x, y, z).
def qmul(a, b):
    return [
        a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
        a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
        a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
        a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
    ]


def qconj(q):
    return [q[0], -q[1], -q[2], -q[3]]


def qrot(q, v):
    u = q[1:4]
    uv = cross(u, v)
    uuv = cross(u, uv)
    return [v[i] + 2.0 * (q[0] * uv[i] + uuv[i]) for i in range(3)]


def q_from_to(f, t):
    d = dot(f, t)
    if d > 1.0 - 1e-6:
        return [1.0, 0.0, 0.0, 0.0]
    v = cross(f, t)
    q = [1.0 + d, v[0], v[1], v[2]]
    l = math.sqrt(sum(x * x for x in q))
    return [x / l for x in q]


def build(pos_bind, quat_bind):
    palm = palm_normal(pos_bind)
    pos = curl(pos_bind, THUMB_CHAIN, THUMB_TARGET, palm)
    for meta, chain, targets in FINGERS:
        pos = curl(pos, chain, targets, palm)
    for aux, tip in AUX_TIP.items():
        pos[aux] = list(pos[tip])

    frames = [[1.0, 0.0, 0.0, 0.0] for _ in range(31)]
    for i in range(31):
        if i == 1:
            frames[i] = list(quat_bind[1])
            continue
        if i in ORIENT_SEG:
            a, b = ORIENT_SEG[i]
            parent = PARENT[i]
            bp = qconj(quat_bind[parent])
            bd = unit(sub(pos_bind[b], pos_bind[a]))
            ld = unit(sub(pos[b], pos[a]))
            flex = q_from_to(unit(qrot(bp, bd)), unit(qrot(qconj(frames[parent]), ld)))
            frames[i] = qmul(qmul(frames[parent], flex), qmul(bp, quat_bind[i]))
        elif i == 0:
            frames[i] = [1.0, 0.0, 0.0, 0.0]
        else:
            parent = PARENT[i]
            frames[i] = qmul(frames[parent], qmul(qconj(quat_bind[parent]), quat_bind[i]))
    for aux, tip in AUX_TIP.items():
        frames[aux] = list(frames[tip])

    out = []
    for i in range(31):
        p = PARENT[i]
        if p < 0:
            out.append(([0.0] * 3, [1.0, 0.0, 0.0, 0.0]))
            continue
        rq = qconj(frames[p])
        out.append((qrot(rq, sub(pos[i], pos[p])), qmul(rq, frames[i])))
    return out, pos


CHAINS = {
    "index": [7, 8, 9],
    "middle": [12, 13, 14],
    "ring": [17, 18, 19],
    "pinky": [22, 23, 24],
}
TIPS = {"index": 10, "middle": 15, "ring": 20, "pinky": 25}


def check(label, out, pos, pos_bind):
    print(f"--- {label} ---", file=sys.stderr)
    for name, chain in CHAINS.items():
        got = [round(flexion(pos, b), 1) for b in chain]
        was = [round(flexion(pos_bind, b), 1) for b in chain]
        print(f"  {name:6} flexion bind {was} -> fist {got}", file=sys.stderr)
    tb = [round(flexion(pos_bind, b), 1) for b in [3, 4]]
    tf = [round(flexion(pos, b), 1) for b in [3, 4]]
    print(f"  thumb  flexion bind {tb} -> fist {tf}", file=sys.stderr)
    # Wrist joint: fingertips should end up near the palm centre.
    for name, chain in CHAINS.items():
        tip = TIPS[name]
        moved = norm(sub(pos[tip], pos_bind[tip])) * 1000.0
        print(f"  {name:6} tip moved {moved:5.1f} mm from bind", file=sys.stderr)
    worst = max(abs(math.sqrt(sum(x * x for x in o[1])) - 1.0) for o in out)
    print(f"  max |quat|-1 = {worst:.2e}", file=sys.stderr)


L, pos_l = build(POS_L, QUAT_L)
R, pos_r = build(POS_R, QUAT_R)
check("left", L, pos_l, POS_L)
check("right", R, pos_r, POS_R)

lines = []
lines.append("// GENERATED by scripts/gen_grip_limit.py from Valve's bind tables in")
lines.append("// bridge/crates/cardboard-bridge/src/ovr_bones.rs. Bare-hand full fist,")
lines.append("// per-joint absolute flexion: MCP 79-90 deg, PIP 89-97 deg, DIP 57-64 deg,")
lines.append("// thumb 40/35 deg. Re-run the generator rather than hand-editing.")
lines.append("//")
lines.append("// SteamVR's default grip limit is the Index controller's fist, which stops")
lines.append("// short of a real full fist and is what caps MCP flexion when a camera")
lines.append("// tracked hand is driven past it.")
lines.append("#pragma once")
lines.append("")
lines.append("// One parent-space bone transform; rot is (w, x, y, z) to match the")
lines.append("// VRBoneTransform_t layout the skeleton component consumes.")
lines.append("struct GripLimitBone {")
lines.append("    float pos[3];")
lines.append("    float rot[4];")
lines.append("};")
lines.append("")
for side, data in (("LEFT", L), ("RIGHT", R)):
    lines.append(f"// {side}: 31 parent-space transforms, index 0 = root at identity.")
    lines.append(f"static const GripLimitBone kGripLimit{side.capitalize()}[31] = {{")
    for p, r in data:
        lines.append(
            "    { { %.6ff, %.6ff, %.6ff }, { %.6ff, %.6ff, %.6ff, %.6ff } },"
            % (p[0], p[1], p[2], r[0], r[1], r[2], r[3])
        )
    lines.append("};")
    lines.append("")
open(OUT_C, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print(f"wrote {OUT_C}", file=sys.stderr)