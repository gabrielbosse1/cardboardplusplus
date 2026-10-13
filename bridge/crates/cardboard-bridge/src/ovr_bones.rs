use crate::net::mediapipe::Landmark;

/// OpenVR bone indices matching hand_tracker.py's BONE_* constants.
pub const BONE_ROOT: usize = 0;
pub const BONE_WRIST: usize = 1;
pub const BONE_THUMB0: usize = 2;
pub const BONE_THUMB1: usize = 3;
pub const BONE_THUMB2: usize = 4;
pub const BONE_THUMB3: usize = 5;
pub const BONE_INDEX0: usize = 6;
pub const BONE_INDEX1: usize = 7;
pub const BONE_INDEX2: usize = 8;
pub const BONE_INDEX3: usize = 9;
pub const BONE_INDEX4: usize = 10;
pub const BONE_MIDDLE0: usize = 11;
pub const BONE_MIDDLE1: usize = 12;
pub const BONE_MIDDLE2: usize = 13;
pub const BONE_MIDDLE3: usize = 14;
pub const BONE_MIDDLE4: usize = 15;
pub const BONE_RING0: usize = 16;
pub const BONE_RING1: usize = 17;
pub const BONE_RING2: usize = 18;
pub const BONE_RING3: usize = 19;
pub const BONE_RING4: usize = 20;
pub const BONE_PINKY0: usize = 21;
pub const BONE_PINKY1: usize = 22;
pub const BONE_PINKY2: usize = 23;
pub const BONE_PINKY3: usize = 24;
pub const BONE_PINKY4: usize = 25;
pub const BONE_AUX_THUMB: usize = 26;
pub const BONE_AUX_INDEX: usize = 27;
pub const BONE_AUX_MIDDLE: usize = 28;
pub const BONE_AUX_RING: usize = 29;
pub const BONE_AUX_PINKY: usize = 30;

pub const NUM_BONES: usize = 31;

/// MediaPipe landmark indices (subset used for bone calculation).
const WRIST: usize = 0;
const THUMB_CMC: usize = 1;
const THUMB_MCP: usize = 2;
const THUMB_IP: usize = 3;
const THUMB_TIP: usize = 4;
const INDEX_MCP: usize = 5;
const INDEX_PIP: usize = 6;
const INDEX_DIP: usize = 7;
const INDEX_TIP: usize = 8;
const MIDDLE_MCP: usize = 9;
const MIDDLE_PIP: usize = 10;
const MIDDLE_DIP: usize = 11;
const MIDDLE_TIP: usize = 12;
const RING_MCP: usize = 13;
const RING_PIP: usize = 14;
const RING_DIP: usize = 15;
const RING_TIP: usize = 16;
const PINKY_MCP: usize = 17;
const PINKY_PIP: usize = 18;
const PINKY_DIP: usize = 19;
const PINKY_TIP: usize = 20;

/// Valve GLB wrist→metacarpal-stub distances (m) for the 2-D overlay path's
/// `short_meta`: Index 25.8mm, Middle 17.9mm, Ring 17.6mm, Pinky 24.5mm.
/// Not the stub→MCP lengths — those are 74/71/66/63 mm and come from
/// `BIND_POS` through `place_metacarpal`.
const META_LEN: [f32; 4] = [0.0258, 0.0179, 0.0176, 0.0245];

/// Vector ops on Landmark (used as 3D points in normalized image space).
fn sub(a: Landmark, b: Landmark) -> Landmark {
    Landmark {
        x: a.x - b.x,
        y: a.y - b.y,
        z: a.z - b.z,
    }
}

fn norm(v: Landmark) -> f32 {
    (v.x * v.x + v.y * v.y + v.z * v.z).sqrt()
}

fn scale(v: Landmark, s: f32) -> Landmark {
    Landmark {
        x: v.x * s,
        y: v.y * s,
        z: v.z * s,
    }
}

fn add(a: Landmark, b: Landmark) -> Landmark {
    Landmark {
        x: a.x + b.x,
        y: a.y + b.y,
        z: a.z + b.z,
    }
}

/// Places the 2-D overlay's metacarpal stub `target_len` from the wrist along
/// the wrist→MCP ray. Preview only; the world path uses `place_metacarpal`.
fn short_meta(wrist: Landmark, mcp: Landmark, target_len: f32) -> Landmark {
    let d = sub(mcp, wrist);
    let l = norm(d);
    if l < 1e-9 {
        return mcp;
    }
    add(wrist, scale(d, target_len / l))
}

/// Model-space joint positions, exposed for tests that need the raw skeleton.
/// The wire format carries parent-relative transforms, so composing them back
/// re-derives these through each bone's live frame and will not reproduce a
/// stub that was placed in model space.
#[cfg(test)]
pub fn compute_positions(
    _image_landmarks: &[Landmark; 21],
    world_landmarks: &[Landmark; 21],
    is_right_hand: bool,
) -> [[f32; 3]; NUM_BONES] {
    let lm: [[f32; 3]; 21] = std::array::from_fn(|i| {
        let l = world_landmarks[i];
        [l.x, -l.y, -l.z]
    });
    let mut positions = [[0.0f32; 3]; NUM_BONES];
    positions[BONE_ROOT] = lm[WRIST];
    positions[BONE_WRIST] = lm[WRIST];
    positions[BONE_THUMB0] = lm[THUMB_CMC];
    positions[BONE_THUMB1] = lm[THUMB_MCP];
    positions[BONE_THUMB2] = lm[THUMB_IP];
    positions[BONE_THUMB3] = lm[THUMB_TIP];
    let bind_pos: &[[f32; 3]; NUM_BONES] = if is_right_hand {
        &BIND_POS_RIGHT
    } else {
        &BIND_POS_LEFT
    };
    let mcp = [
        (BONE_INDEX0, BONE_INDEX1, lm[INDEX_MCP]),
        (BONE_MIDDLE0, BONE_MIDDLE1, lm[MIDDLE_MCP]),
        (BONE_RING0, BONE_RING1, lm[RING_MCP]),
        (BONE_PINKY0, BONE_PINKY1, lm[PINKY_MCP]),
    ];
    for (bone, mcp_bone, p) in mcp {
        positions[bone] =
            place_metacarpal(bind_pos, [1.0, 0.0, 0.0, 0.0], bone, mcp_bone, p);
        positions[mcp_bone] = p;
    }
    let chain = [
        (BONE_INDEX1, INDEX_PIP, INDEX_DIP, INDEX_TIP),
        (BONE_MIDDLE1, MIDDLE_PIP, MIDDLE_DIP, MIDDLE_TIP),
        (BONE_RING1, RING_PIP, RING_DIP, RING_TIP),
        (BONE_PINKY1, PINKY_PIP, PINKY_DIP, PINKY_TIP),
    ];
    for (b1, pip, dip, tip) in chain {
        positions[b1 + 1] = lm[pip];
        positions[b1 + 2] = lm[dip];
        positions[b1 + 3] = lm[tip];
    }
    positions
}

/// Compute OpenVR bones from 21 MediaPipe landmarks.
///
/// The algorithm does NOT modify the original landmarks. It adds 10 new bones:
/// - BONE_ROOT(0), BONE_WRIST(1): both at wrist position
/// - 4 metacarpals (INDEX0=6, MIDDLE0=11, RING0=16, PINKY0=21): stub on Valve's metacarpal axis
/// - 5 aux bones (26-30): track their respective tips
///
/// Finger chain matches Valve's GLB node layout (meta, _0, _1, _2, _end):
/// bone1←MCP, bone2←PIP, bone3←DIP, bone4←TIP. All 21 original landmarks
/// are copied directly into the corresponding bone slots.
pub fn compute_bones(landmarks: &[Landmark; 21]) -> [Landmark; NUM_BONES] {
    let w = landmarks[WRIST];
    let mut bones = [Landmark::default(); NUM_BONES];

    // Root & wrist = wrist
    bones[BONE_ROOT] = w;
    bones[BONE_WRIST] = w;

    // Thumb: direct map (CMC, MCP, IP, TIP)
    bones[BONE_THUMB0] = landmarks[THUMB_CMC];
    bones[BONE_THUMB1] = landmarks[THUMB_MCP];
    bones[BONE_THUMB2] = landmarks[THUMB_IP];
    bones[BONE_THUMB3] = landmarks[THUMB_TIP];

    // Index finger
    bones[BONE_INDEX0] = short_meta(w, landmarks[INDEX_MCP], META_LEN[0]);
    bones[BONE_INDEX1] = landmarks[INDEX_MCP];
    bones[BONE_INDEX2] = landmarks[INDEX_PIP];
    bones[BONE_INDEX3] = landmarks[INDEX_DIP];
    bones[BONE_INDEX4] = landmarks[INDEX_TIP];

    // Middle finger
    bones[BONE_MIDDLE0] = short_meta(w, landmarks[MIDDLE_MCP], META_LEN[1]);
    bones[BONE_MIDDLE1] = landmarks[MIDDLE_MCP];
    bones[BONE_MIDDLE2] = landmarks[MIDDLE_PIP];
    bones[BONE_MIDDLE3] = landmarks[MIDDLE_DIP];
    bones[BONE_MIDDLE4] = landmarks[MIDDLE_TIP];

    // Ring finger
    bones[BONE_RING0] = short_meta(w, landmarks[RING_MCP], META_LEN[2]);
    bones[BONE_RING1] = landmarks[RING_MCP];
    bones[BONE_RING2] = landmarks[RING_PIP];
    bones[BONE_RING3] = landmarks[RING_DIP];
    bones[BONE_RING4] = landmarks[RING_TIP];

    // Pinky finger
    bones[BONE_PINKY0] = short_meta(w, landmarks[PINKY_MCP], META_LEN[3]);
    bones[BONE_PINKY1] = landmarks[PINKY_MCP];
    bones[BONE_PINKY2] = landmarks[PINKY_PIP];
    bones[BONE_PINKY3] = landmarks[PINKY_DIP];
    bones[BONE_PINKY4] = landmarks[PINKY_TIP];

    // Aux bones track their respective tips
    bones[BONE_AUX_THUMB] = bones[BONE_THUMB3];
    bones[BONE_AUX_INDEX] = bones[BONE_INDEX4];
    bones[BONE_AUX_MIDDLE] = bones[BONE_MIDDLE4];
    bones[BONE_AUX_RING] = bones[BONE_RING4];
    bones[BONE_AUX_PINKY] = bones[BONE_PINKY4];

    bones
}

/// Indexes of the extra bones (not original MediaPipe landmarks) for overlay drawing.
pub const EXTRA_BONE_INDEXES: &[usize] = &[
    BONE_ROOT,
    BONE_WRIST,
    BONE_INDEX0,
    BONE_MIDDLE0,
    BONE_RING0,
    BONE_PINKY0,
    BONE_AUX_THUMB,
    BONE_AUX_INDEX,
    BONE_AUX_MIDDLE,
    BONE_AUX_RING,
    BONE_AUX_PINKY,
];

/// Connections between extra bones to draw in blue.
/// Each tuple is (from, to) bone indices — only between extra bones.
pub const EXTRA_CONNECTIONS: &[[usize; 2]] = &[
    [BONE_WRIST, BONE_INDEX0],
    [BONE_WRIST, BONE_MIDDLE0],
    [BONE_WRIST, BONE_RING0],
    [BONE_WRIST, BONE_PINKY0],
    [BONE_INDEX4, BONE_AUX_INDEX],
    [BONE_MIDDLE4, BONE_AUX_MIDDLE],
    [BONE_RING4, BONE_AUX_RING],
    [BONE_PINKY4, BONE_AUX_PINKY],
    [BONE_THUMB3, BONE_AUX_THUMB],
];

/// Full bone transform: position + orientation quaternion for SteamVR skeletal input.
#[derive(Debug, Clone, Copy)]
pub struct BoneTransform {
    pub position: [f32; 3],
    /// Quaternion (w, x, y, z).
    pub orientation: [f32; 4],
}

impl Default for BoneTransform {
    fn default() -> Self {
        Self {
            position: [0.0; 3],
            orientation: [1.0, 0.0, 0.0, 0.0],
        }
    }
}

/// Cross product of two 3-vectors.
fn vcross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

fn vdot(a: [f32; 3], b: [f32; 3]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn vnorm(v: [f32; 3]) -> f32 {
    (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt()
}

fn vnormalize(v: [f32; 3]) -> [f32; 3] {
    let l = vnorm(v);
    if l < 1e-9 {
        return [0.0; 3];
    }
    [v[0] / l, v[1] / l, v[2] / l]
}

fn vsub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn vscale(v: [f32; 3], s: f32) -> [f32; 3] {
    [v[0] * s, v[1] * s, v[2] * s]
}

/// Quaternion from two unit vectors (rotation that maps `from` to `to`).
fn quat_from_to(from: [f32; 3], to: [f32; 3]) -> [f32; 4] {
    let d = vdot(from, to);
    if d > 1.0 - 1e-6 {
        return [1.0, 0.0, 0.0, 0.0];
    }
    if d < -1.0 + 1e-6 {
        // 180-degree rotation: pick an arbitrary perpendicular axis
        let up = if from[0].abs() < 0.9 {
            [1.0, 0.0, 0.0]
        } else {
            [0.0, 1.0, 0.0]
        };
        let v = vnormalize(vcross(from, up));
        return [0.0, v[0], v[1], v[2]];
    }
    let v = vcross(from, to);
    let w = 1.0 + d;
    let q = [w, v[0], v[1], v[2]];
    let l = (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]).sqrt();
    [q[0] / l, q[1] / l, q[2] / l, q[3] / l]
}

/// Best-fit palm-plane normal over the wrist plus all four MCP joints
/// (Newell's method), oriented so it points out of the palm.
///
/// Newell's winding follows the ring's order, and that order runs opposite
/// ways on the two hands, so the raw result is the palm on one side and the
/// back of the hand on the other. `palm_sign` cannot fix that: it is derived
/// from the *bind* tables, and this function is also handed live landmarks that
/// have already been through a different axis flip. The thumb settles it
/// without reference to either — the palm always faces away from it.
///
/// Returns `None` when the polygon collapses, which happens when a closed hand
/// turns edge-on to the camera and every MCP lines up.
///
/// A two-point cross product (wrist x index MCP) is the obvious cheaper
/// construction and it is wrong twice over: the wrist→index-MCP and
/// wrist→middle-MCP directions are only ~17.6 deg apart, so 1/sin amplifies
/// landmark noise ~3.3x, and the pair sits at ~90 mm radius on a curved knuckle
/// ridge where the palm plane wants them at the palm surface. Five points spread
/// across the palm average that curvature out.
fn palm_plane_normal(p: &[[f32; 3]; NUM_BONES]) -> Option<[f32; 3]> {
    let ring = [
        p[BONE_WRIST],
        p[BONE_INDEX1],
        p[BONE_MIDDLE1],
        p[BONE_RING1],
        p[BONE_PINKY1],
    ];
    let mut n = [0.0f32; 3];
    for i in 0..ring.len() {
        let a = ring[i];
        let b = ring[(i + 1) % ring.len()];
        n[0] += a[1] * b[2] - a[2] * b[1];
        n[1] += a[2] * b[0] - a[0] * b[2];
        n[2] += a[0] * b[1] - a[1] * b[0];
    }
    let l = vnorm(n);
    if l < 1e-9 {
        return None;
    }
    let n = [n[0] / l, n[1] / l, n[2] / l];
    // Orient out of the palm using the thumb, which sits inside the hand's
    // silhouette on both sides.
    let thumb = vnormalize(vsub(p[BONE_THUMB0], p[BONE_WRIST]));
    if vdot(n, thumb) > 0.0 {
        Some([-n[0], -n[1], -n[2]])
    } else {
        Some(n)
    }
}

/// Rotation taking the bind basis (finger, palm) onto the live basis.
/// Both pairs are orthogonalized against the finger axis first; the third
/// axis is finger × palm so handedness matches on both sides.
fn quat_from_basis(
    finger_bind: [f32; 3],
    palm_bind: [f32; 3],
    finger_live: [f32; 3],
    palm_live: [f32; 3],
) -> [f32; 4] {
    let f0 = vnormalize(finger_bind);
    let f1 = vnormalize(finger_live);
    let ortho =
        |p: [f32; 3], f: [f32; 3]| -> [f32; 3] { vnormalize(vsub(p, vscale(f, vdot(p, f)))) };
    let p0 = ortho(palm_bind, f0);
    let p1 = ortho(palm_live, f1);
    let s0 = vcross(f0, p0);
    let s1 = vcross(f1, p1);
    // R = [s1 p1 f1] · [s0 p0 f0]ᵀ (columns = basis vectors).
    let mut m = [[0.0f32; 3]; 3];
    let c0 = [s0, p0, f0];
    let c1 = [s1, p1, f1];
    for r in 0..3 {
        for col in 0..3 {
            m[r][col] = c1[0][r] * c0[0][col] + c1[1][r] * c0[1][col] + c1[2][r] * c0[2][col];
        }
    }
    mat_to_quat(m)
}

/// Convert a 3×3 rotation matrix (row-major) to a quaternion (w,x,y,z).
///
/// The four branches below each recover the quaternion from a different
/// component pair, and they do not agree on its sign: `q` and `-q` are the same
/// rotation, so whichever branch runs picks an arbitrary representative. The
/// result is biased toward `w >= 0` here, which is the best a stateless
/// function can do; `match_sign_to_previous` is what actually keeps a stream
/// continuous.
fn mat_to_quat(m: [[f32; 3]; 3]) -> [f32; 4] {
    let (m00, m01, m02) = (m[0][0], m[0][1], m[0][2]);
    let (m10, m11, m12) = (m[1][0], m[1][1], m[1][2]);
    let (m20, m21, m22) = (m[2][0], m[2][1], m[2][2]);
    let trace = m00 + m11 + m22;
    let q = if trace > 0.0 {
        let s = 0.5 / (trace + 1.0).sqrt();
        [0.25 / s, (m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s]
    } else if m00 > m11 && m00 > m22 {
        let s = 2.0 * (1.0 + m00 - m11 - m22).sqrt();
        [(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s]
    } else if m11 > m22 {
        let s = 2.0 * (1.0 + m11 - m00 - m22).sqrt();
        [(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s]
    } else {
        let s = 2.0 * (1.0 + m22 - m00 - m11).sqrt();
        [(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s]
    };
    match_sign_to(q, [1.0, 0.0, 0.0, 0.0])
}

/// Pick the representative of the `q` / `-q` pair that continues the previous
/// frame, so a quaternion stream never crosses a sign boundary.
///
/// `q` and `-q` are the same rotation, which is why this is easy to miss: the
/// skeleton geometry is identical either way, and every check on the resulting
/// pose passes. It matters because the consumer interpolates. SteamVR slerps
/// between the frames it is handed, and the short way from `q` to `-q` is a
/// full 360°, so a lone sign flip reads as the hand spinning once.
///
/// No stateless rule avoids this. `w >= 0` flips wherever `w` crosses zero, and
/// "largest component positive" flips wherever the largest component changes —
/// a roll through a true 180° passes `w` straight through zero at 172° and
/// 352° of a full turn, and both rules flip there. Only continuity with the
/// previous frame cannot flip mid-motion, because the previous frame is what
/// defines the sign.
fn match_sign_to_previous(q: [f32; 4], prev: [f32; 4]) -> [f32; 4] {
    let dot = q[0] * prev[0] + q[1] * prev[1] + q[2] * prev[2] + q[3] * prev[3];
    if dot < 0.0 {
        [-q[0], -q[1], -q[2], -q[3]]
    } else {
        q
    }
}

/// Pick the representative of the `q` / `-q` pair that agrees with a reference
/// rotation, for callers holding no previous frame. `reference` is any
/// quaternion known to express the same rotation; pass the identity to bias
/// toward `w >= 0`.
fn match_sign_to(q: [f32; 4], reference: [f32; 4]) -> [f32; 4] {
    match_sign_to_previous(q, reference)
}

/// Quaternion product q * r.
fn quat_mul(q: [f32; 4], r: [f32; 4]) -> [f32; 4] {
    [
        q[0] * r[0] - q[1] * r[1] - q[2] * r[2] - q[3] * r[3],
        q[0] * r[1] + q[1] * r[0] + q[2] * r[3] - q[3] * r[2],
        q[0] * r[2] - q[1] * r[3] + q[2] * r[0] + q[3] * r[1],
        q[0] * r[3] + q[1] * r[2] - q[2] * r[1] + q[3] * r[0],
    ]
}

/// OpenVR bone parent hierarchy (parent index for each bone, -1 for root).
/// Matches the vr_glove GLB node tree: aux bones hang off the root, every
/// finger bone chains from the wrist through its segments.
const PARENT: [i32; NUM_BONES] = [
    -1, 0, 1, 2, 3, 4, // root, wrist, thumb0-3
    1, 6, 7, 8, 9, // index0-4
    1, 11, 12, 13, 14, // middle0-4
    1, 16, 17, 18, 19, // ring0-4
    1, 21, 22, 23, 24, // pinky0-4
    0, 0, 0, 0, 0, // aux (children of root per GLB)
];

/// Metacarpal bones are placed by `place_metacarpal` and are deliberately
/// left alone here.
const METACARPAL_BONES: [usize; 4] = [
    BONE_INDEX0,
    BONE_MIDDLE0,
    BONE_RING0,
    BONE_PINKY0,
];

/// Rescale every phalange to Valve's bind length along its live direction.
///
/// MediaPipe's world landmarks are not a metric measurement of the hand. On a
/// real capture the phalanges come out at roughly 0.55-0.77x Valve's bind
/// lengths — a pinky MCP-to-PIP of 16 mm against a bind 30 mm. SteamVR skins
/// the glove with Valve's inverse-bind matrices, so a bone arriving at the
/// wrong length drags the mesh with it, and a short bone also sweeps a shorter
/// arc for the same joint angle: a 30 deg abduction moves the pinky tip 21 mm
/// at capture scale against 33 mm at bind scale. That deficit is what stops the
/// fingers spreading as far as a real hand.
///
/// Each segment keeps its direction, so no joint angle changes and the whole
/// orientation pass downstream is untouched — only the lengths move. Walking
/// each chain from the wrist outward is what makes that true: the child is
/// placed relative to the parent that has already been placed, so the angles
/// between consecutive segments are preserved exactly.
///
/// Mirrors hand_tracker.py `GloveMesh.normalize`, which the Rust port lost.
fn normalize_segments(positions: &mut [[f32; 3]; NUM_BONES], is_right_hand: bool) {
    let bind_pos = if is_right_hand {
        &BIND_POS_RIGHT
    } else {
        &BIND_POS_LEFT
    };
    let orig = *positions;
    for i in 2..NUM_BONES {
        if METACARPAL_BONES.contains(&i) {
            continue;
        }
        let Some((a, b)) = ORIENT_SEG[i] else { continue };
        let want = vnorm(vsub(bind_pos[b], bind_pos[a]));
        // Direction from the landmarks, never from the position this loop has
        // already moved. Rescaling a segment about its parent carries the next
        // segment's start point with it, so reading the direction back out of
        // the moved chain bends every joint downstream of it.
        let d = vsub(orig[b], orig[a]);
        let l = vnorm(d);
        if want < 1e-9 || l < 1e-9 {
            continue;
        }
        let k = want / l;
        positions[b] = [
            positions[a][0] + d[0] * k,
            positions[a][1] + d[1] * k,
            positions[a][2] + d[2] * k,
        ];
    }
}

/// Places a metacarpal stub back from its MCP along Valve's metacarpal axis,
/// rotated by `basis` so it follows a wrist turn.
///
/// The length is the bind stub→MCP distance read straight off `BIND_POS`,
/// which is what makes the live segment match the bind table instead of
/// guessing it. Valve's own wrist→stub distances are much shorter than its
/// stub→MCP ones (26 vs 74 mm on the index), so the two are not
/// interchangeable and only the bind table settles which one is meant.
fn place_metacarpal(
    bind_pos: &[[f32; 3]; NUM_BONES],
    basis: [f32; 4],
    stub: usize,
    mcp_bone: usize,
    mcp: [f32; 3],
) -> [f32; 3] {
    let axis = vsub(bind_pos[mcp_bone], bind_pos[stub]);
    let len = vnorm(axis);
    if len < 1e-9 {
        return mcp;
    }
    vsub(mcp, vscale(quat_rot_vec(basis, vscale(axis, 1.0 / len)), len))
}

/// Bind-pose segment used to orient each bone: model-space direction from
const ORIENT_SEG: [Option<(usize, usize)>; NUM_BONES] = [
    None,                             // 0 root
    None, // 1 wrist: taken by the 3-DOF basis branch below, never read here
    Some((BONE_THUMB0, BONE_THUMB1)),
    Some((BONE_THUMB1, BONE_THUMB2)),
    Some((BONE_THUMB2, BONE_THUMB3)),
    None, // 5 thumb tip
    Some((BONE_INDEX0, BONE_INDEX1)),
    Some((BONE_INDEX1, BONE_INDEX2)),
    Some((BONE_INDEX2, BONE_INDEX3)),
    Some((BONE_INDEX3, BONE_INDEX4)),
    None, // 10
    Some((BONE_MIDDLE0, BONE_MIDDLE1)),
    Some((BONE_MIDDLE1, BONE_MIDDLE2)),
    Some((BONE_MIDDLE2, BONE_MIDDLE3)),
    Some((BONE_MIDDLE3, BONE_MIDDLE4)),
    None, // 15
    Some((BONE_RING0, BONE_RING1)),
    Some((BONE_RING1, BONE_RING2)),
    Some((BONE_RING2, BONE_RING3)),
    Some((BONE_RING3, BONE_RING4)),
    None, // 20
    Some((BONE_PINKY0, BONE_PINKY1)),
    Some((BONE_PINKY1, BONE_PINKY2)),
    Some((BONE_PINKY2, BONE_PINKY3)),
    Some((BONE_PINKY3, BONE_PINKY4)),
    None, // 25
    None,
    None,
    None,
    None,
    None, // 26-30 aux
];

/// Valve vr_glove bind pose: model-space joint positions (meters).
/// Extracted from SteamVR resources/skeletons/vr_glove_{left,right}_skeleton.glb.
const BIND_POS_LEFT: [[f32; 3]; NUM_BONES] = [
    [0.000000, 0.000000, 0.000000],
    [0.000160, -0.000032, -0.000626],
    [-0.017754, 0.029146, 0.024673],
    [-0.028318, 0.054373, 0.054416],
    [-0.039281, 0.060081, 0.084492],
    [-0.049867, 0.056092, 0.112777],
    [-0.001397, 0.021041, 0.014161],
    [0.011200, 0.037357, 0.085022],
    [-0.001070, 0.038485, 0.126517],
    [-0.018233, 0.037278, 0.148956],
    [-0.034598, 0.035540, 0.164767],
    [0.002337, 0.007088, 0.015693],
    [0.016529, 0.009429, 0.085104],
    [0.005480, 0.009177, 0.126772],
    [-0.012561, 0.007871, 0.154690],
    [-0.032113, 0.006542, 0.171613],
    [0.000673, -0.006577, 0.015722],
    [0.009861, -0.013241, 0.080713],
    [-0.002356, -0.019657, 0.118610],
    [-0.017869, -0.023238, 0.142235],
    [-0.034683, -0.025393, 0.156924],
    [-0.002318, -0.019013, 0.014588],
    [-0.002127, -0.037681, 0.074607],
    [-0.008006, -0.043301, 0.103353],
    [-0.016008, -0.045652, 0.119280],
    [-0.027097, -0.046290, 0.133467],
    [-0.039281, 0.060081, 0.084492],
    [-0.018233, 0.037278, 0.148956],
    [-0.012561, 0.007871, 0.154690],
    [-0.017869, -0.023238, 0.142235],
    [-0.016008, -0.045652, 0.119280],
];

const BIND_POS_RIGHT: [[f32; 3]; NUM_BONES] = [
    [0.000000, 0.000000, 0.000000],
    [-0.000160, -0.000032, -0.000626],
    [0.017754, 0.029146, 0.024673],
    [0.028318, 0.054373, 0.054416],
    [0.039281, 0.060081, 0.084492],
    [0.049867, 0.056092, 0.112777],
    [0.001397, 0.021041, 0.014161],
    [-0.011200, 0.037357, 0.085022],
    [0.001070, 0.038485, 0.126518],
    [0.018233, 0.037278, 0.148956],
    [0.034598, 0.035540, 0.164767],
    [-0.002337, 0.007088, 0.015693],
    [-0.016529, 0.009429, 0.085104],
    [-0.005480, 0.009177, 0.126772],
    [0.012561, 0.007871, 0.154690],
    [0.032113, 0.006542, 0.171613],
    [-0.000673, -0.006577, 0.015722],
    [-0.009861, -0.013241, 0.080713],
    [0.002356, -0.019657, 0.118610],
    [0.017869, -0.023238, 0.142235],
    [0.034683, -0.025393, 0.156924],
    [0.002318, -0.019013, 0.014588],
    [0.002127, -0.037681, 0.074607],
    [0.008006, -0.043301, 0.103353],
    [0.016008, -0.045652, 0.119280],
    [0.027097, -0.046290, 0.133467],
    [0.039281, 0.060081, 0.084492],
    [0.018233, 0.037278, 0.148956],
    [0.012561, 0.007871, 0.154690],
    [0.017869, -0.023238, 0.142235],
    [0.016008, -0.045652, 0.119280],
];

/// Valve bind orientations (w, x, y, z), model space, same GLB source.
const BIND_QUAT_LEFT: [[f32; 4]; NUM_BONES] = [
    [1.000000, 0.000000, 0.000000, 0.000000],
    [1.000000, 0.000000, 0.000000, 0.000000],
    [0.276387, 0.541194, 0.182029, 0.773036],
    [0.077625, 0.570436, 0.042748, 0.816547],
    [-0.048614, 0.569107, -0.045037, 0.819589],
    [-0.048614, 0.569107, -0.045037, 0.819589],
    [0.550753, 0.531056, -0.351434, 0.539578],
    [0.383965, 0.459177, -0.496984, 0.628279],
    [0.263812, 0.356241, -0.571378, 0.690668],
    [0.209555, 0.312326, -0.597228, 0.708419],
    [0.209555, 0.312326, -0.597228, 0.708419],
    [0.533423, 0.561750, -0.419737, 0.472988],
    [0.410759, 0.450700, -0.537398, 0.582537],
    [0.311924, 0.362682, -0.603095, 0.638312],
    [0.221141, 0.271171, -0.647061, 0.677403],
    [0.221141, 0.271171, -0.647061, 0.677403],
    [0.516692, 0.550143, -0.495548, 0.429888],
    [-0.389011, -0.444083, 0.625723, -0.509834],
    [-0.318896, -0.355027, 0.678577, -0.558386],
    [-0.237415, -0.262352, 0.721629, -0.595027],
    [-0.237415, -0.262352, 0.721629, -0.595027],
    [-0.485758, -0.515327, 0.615016, -0.346746],
    [-0.459637, -0.436281, 0.652809, -0.415010],
    [-0.392908, -0.350822, 0.709932, -0.467486],
    [-0.349000, -0.265484, 0.739031, -0.511421],
    [-0.349000, -0.265484, 0.739031, -0.511421],
    [-0.048614, 0.569107, -0.045037, 0.819589],
    [0.209555, 0.312326, -0.597228, 0.708419],
    [0.221141, 0.271171, -0.647061, 0.677403],
    [-0.237415, -0.262352, 0.721629, -0.595027],
    [-0.349000, -0.265484, 0.739030, -0.511421],
];

const BIND_QUAT_RIGHT: [[f32; 4]; NUM_BONES] = [
    [1.000000, 0.000000, 0.000000, 0.000000],
    [1.000000, 0.000000, 0.000000, 0.000000],
    [0.541194, -0.276387, 0.773036, -0.182029],
    [0.570436, -0.077625, 0.816547, -0.042748],
    [0.569107, 0.048614, 0.819589, 0.045037],
    [0.569107, 0.048614, 0.819589, 0.045037],
    [0.531056, -0.550753, 0.539578, 0.351434],
    [0.459177, -0.383965, 0.628279, 0.496984],
    [0.356241, -0.263812, 0.690668, 0.571378],
    [0.312325, -0.209555, 0.708419, 0.597228],
    [0.312325, -0.209555, 0.708419, 0.597228],
    [0.561750, -0.533423, 0.472988, 0.419737],
    [0.450700, -0.410759, 0.582537, 0.537398],
    [0.362682, -0.311924, 0.638312, 0.603095],
    [0.271171, -0.221141, 0.677403, 0.647061],
    [0.271171, -0.221141, 0.677403, 0.647061],
    [0.550143, -0.516692, 0.429888, 0.495548],
    [0.444083, -0.389011, 0.509834, 0.625723],
    [0.355027, -0.318896, 0.558386, 0.678577],
    [0.262352, -0.237415, 0.595027, 0.721629],
    [0.262352, -0.237415, 0.595027, 0.721629],
    [0.515327, -0.485758, 0.346746, 0.615016],
    [0.436281, -0.459637, 0.415010, 0.652809],
    [0.350822, -0.392908, 0.467486, 0.709932],
    [0.265484, -0.349000, 0.511421, 0.739031],
    [0.265484, -0.349000, 0.511421, 0.739031],
    [0.569107, 0.048614, 0.819589, 0.045037],
    [0.312326, -0.209555, 0.708419, 0.597228],
    [0.271171, -0.221141, 0.677403, 0.647061],
    [0.262352, -0.237415, 0.595027, 0.721629],
    [0.265484, -0.349000, 0.511421, 0.739030],
];

/// Compute OpenVR bones from MediaPipe landmarks.
///
/// `world_landmarks` are metric landmarks from MediaPipe (wrist-relative).
/// `image_landmarks` are normalized [0,1] image coordinates used to recover
/// the wrist's absolute position in camera space via a pinhole model.
/// `is_right_hand` selects Valve's left or right bind-pose tables.
///
/// Bone orientations are bind+delta: Valve's GLB bind quat pre-multiplied by
/// the shortest rotation taking each bind segment onto the live segment
/// (same as hand_tracker.py's live_mats). SteamVR skins with those same
/// inverse-bind matrices — inventing axes here rolls the mesh against the
/// landmarks even when joint positions are correct.
///
/// Wire convention for the 0x13 skeleton packet (matches Valve's
/// handskeletonsimulation sample driver: SteamVR composes parent-relative
/// transforms itself):
/// - bone 0 (root) carries the device pose in TRACKING space: the absolute
///   wrist anchor with the head rotation applied (see apply_head_pose) plus
///   the head quat as its orientation. The driver copies both fields into
///   the SteamVR device pose verbatim and resets bone 0 to (0, identity)
///   before pushing the skeleton.
/// - bone 1 (wrist) carries the palm orientation relative to the root, so
///   the whole glove rotates as a unit when the palm rolls.
/// - bones 2..30 are PARENT-relative (position offset + orientation, both
///   in the parent bone's frame) at MediaPipe's metric scale, with
///   metacarpal stubs at Valve's lengths. Positions are never mirrored: true side is preserved.
///   Parent-relative transforms stay in the device frame — SteamVR composes
///   them under the device pose itself.
#[cfg(test)]
pub fn compute_world_bones(
    image_landmarks: &[Landmark; 21],
    world_landmarks: &[Landmark; 21],
    is_right_hand: bool,
) -> [BoneTransform; NUM_BONES] {
    compute_world_bones_with_palm(image_landmarks, world_landmarks, is_right_hand, None, None)
}

/// `compute_world_bones` plus the previous frame's orientations, for a caller
/// that sends frames in sequence and wants the sign continuity that requires.
pub fn compute_world_bones_tracked(
    image_landmarks: &[Landmark; 21],
    world_landmarks: &[Landmark; 21],
    is_right_hand: bool,
    palm: Option<[f32; 3]>,
    prev_orientations: &[[f32; 4]; NUM_BONES],
) -> [BoneTransform; NUM_BONES] {
    compute_world_bones_with_palm(
        image_landmarks,
        world_landmarks,
        is_right_hand,
        palm,
        Some(prev_orientations),
    )
}

/// Same skeleton with a caller-supplied palm normal in MediaPipe axes (same
/// convention as `world_landmarks`; converted to OpenVR axes inside). A
/// missing or degenerate override falls back to the position-derived normal.
///
/// `prev_orientations` is the previous frame's orientations, straight from the
/// last call. Passing it keeps the quaternion signs continuous, which the
/// consumer needs because it interpolates between frames; `None` is correct
/// only for a stateless one-shot (and for tests that do not check continuity).
pub fn compute_world_bones_with_palm(
    image_landmarks: &[Landmark; 21],
    world_landmarks: &[Landmark; 21],
    is_right_hand: bool,
    palm: Option<[f32; 3]>,
    prev_orientations: Option<&[[f32; 4]; NUM_BONES]>,
) -> [BoneTransform; NUM_BONES] {
    // Convert world landmarks to OpenVR space:
    //   MediaPipe: +X right, +Y down, +Z away from camera (smaller = closer)
    //   OpenVR:    +X right, +Y up,   +Z toward the user (nearer the head)
    // Y and Z both negate: nearer-camera stays nearer-user, so curl toward
    // the palm (closer to camera) maps toward +Z OpenVR.
    let lm: [[f32; 3]; 21] = std::array::from_fn(|i| {
        let l = world_landmarks[i];
        [l.x, -l.y, -l.z]
    });
    let w = lm[WRIST];

    // Depth is fixed: apparent-size depth estimation proved too noisy
    // (MediaPipe image z barely moves with distance and the span signal
    // jumps frame to frame). Typical phone at arm's length.
    let wrist_img = &image_landmarks[0];
    let depth = 0.50_f32;

    // Absolute wrist position in camera space (OpenVR axes).
    let tan_hfov_half = 0.839_f32; // tan(40°) ≈ 80° HFOV
    let tan_vfov_half = 0.58_f32; // tan(30°) ≈ 60° VFOV
    // Principal-point trim: phone cameras are rarely centered on the
    // sensor middle, and the headset mount adds its own offset. CY > 0.5
    // shifts the render up; tune in 0.01 steps if hands sit high/low.
    const IMAGE_CX: f32 = 0.5;
    const IMAGE_CY: f32 = 0.55;
    let wx = (wrist_img.x - IMAGE_CX) * 2.0 * depth * tan_hfov_half;
    let wy = -(wrist_img.y - IMAGE_CY) * 2.0 * depth * tan_vfov_half;
    let wz = -depth; // OpenVR: -Z = forward
    let wrist_abs = [wx, wy, wz];

    let bind_pos: &[[f32; 3]; NUM_BONES] = if is_right_hand {
        &BIND_POS_RIGHT
    } else {
        &BIND_POS_LEFT
    };
    let bind_q: &[[f32; 4]; NUM_BONES] = if is_right_hand {
        &BIND_QUAT_RIGHT
    } else {
        &BIND_QUAT_LEFT
    };

    let mut positions = [[0.0f32; 3]; NUM_BONES];

    // LOCAL (wrist-relative) skeleton first; the absolute anchor goes into
    // bone 0 at the end. Orientations are translation-invariant.
    positions[BONE_ROOT] = w;
    positions[BONE_WRIST] = w;

    // Thumb: direct map of scaled landmarks
    positions[BONE_THUMB0] = lm[THUMB_CMC];
    positions[BONE_THUMB1] = lm[THUMB_MCP];
    positions[BONE_THUMB2] = lm[THUMB_IP];
    positions[BONE_THUMB3] = lm[THUMB_TIP];

    positions[BONE_INDEX1] = lm[INDEX_MCP];
    positions[BONE_INDEX2] = lm[INDEX_PIP];
    positions[BONE_INDEX3] = lm[INDEX_DIP];
    positions[BONE_INDEX4] = lm[INDEX_TIP];

    positions[BONE_MIDDLE1] = lm[MIDDLE_MCP];
    positions[BONE_MIDDLE2] = lm[MIDDLE_PIP];
    positions[BONE_MIDDLE3] = lm[MIDDLE_DIP];
    positions[BONE_MIDDLE4] = lm[MIDDLE_TIP];

    positions[BONE_RING1] = lm[RING_MCP];
    positions[BONE_RING2] = lm[RING_PIP];
    positions[BONE_RING3] = lm[RING_DIP];
    positions[BONE_RING4] = lm[RING_TIP];

    positions[BONE_PINKY1] = lm[PINKY_MCP];
    positions[BONE_PINKY2] = lm[PINKY_PIP];
    positions[BONE_PINKY3] = lm[PINKY_DIP];
    positions[BONE_PINKY4] = lm[PINKY_TIP];

    // Metacarpal stubs, one per finger.
    //
    // The stub sits one metacarpal length back from the MCP along Valve's
    // metacarpal axis, which is what makes the stub→MCP segment match the bind
    // table and leaves the metacarpal contributing no spurious flexion.
    // Landing it on the wrist→MCP ray instead (the obvious construction) puts
    // it on a different ray and leaves degrees of standing error that every bone
    // below inherits — the whole finger chain then renders rotated.
    //
    // Valve's axis is in model space, so it is mapped through the same rotation
    // that carries the bind hand onto this one before being applied. A stub
    // pinned to model space cannot follow a wrist turn: the MCP moves with the
    // hand while the offset does not, and the segment shears.
    // One palm normal for the whole skeleton. Deriving it twice — once for the
    // stub basis and again for the wrist — leaves two normals to disagree, and
    // the stubs then shear against the wrist instead of moving with it.
    let palm_bind = palm_plane_normal(bind_pos);
    let palm_live = match palm {
        // Wire override arrives in MediaPipe axes; convert it into model space
        // and orient it out of the palm the same way, since its winding comes
        // from the caller rather than from these tables.
        Some(p) if vnorm(p) > 1e-9 => {
            let n = vnormalize([p[0], -p[1], -p[2]]);
            let thumb = vnormalize(vsub(positions[BONE_THUMB0], positions[BONE_WRIST]));
            Some(if vdot(n, thumb) > 0.0 {
                [-n[0], -n[1], -n[2]]
            } else {
                n
            })
        }
        _ => palm_plane_normal(&positions),
    };
    let basis = match (palm_bind, palm_live) {
        (Some(pb), Some(pl)) => quat_from_basis(
            vsub(bind_pos[BONE_MIDDLE1], bind_pos[BONE_WRIST]),
            pb,
            vsub(positions[BONE_MIDDLE1], positions[BONE_WRIST]),
            pl,
        ),
        _ => [1.0, 0.0, 0.0, 0.0],
    };
    for (bone, mcp_bone) in [
        (BONE_INDEX0, BONE_INDEX1),
        (BONE_MIDDLE0, BONE_MIDDLE1),
        (BONE_RING0, BONE_RING1),
        (BONE_PINKY0, BONE_PINKY1),
    ] {
        positions[bone] =
            place_metacarpal(bind_pos, basis, bone, mcp_bone, positions[mcp_bone]);
    }

    normalize_segments(&mut positions, is_right_hand);

    // Aux bones = their tips
    positions[BONE_AUX_THUMB] = positions[BONE_THUMB3];
    positions[BONE_AUX_INDEX] = positions[BONE_INDEX4];
    positions[BONE_AUX_MIDDLE] = positions[BONE_MIDDLE4];
    positions[BONE_AUX_RING] = positions[BONE_RING4];
    positions[BONE_AUX_PINKY] = positions[BONE_PINKY4];

    // Model-space orientations: Valve bind quat rotated by the delta that
    // takes each bind segment onto the live segment (hand_tracker.py's
    // live_mats). SteamVR skins with the same inverse-bind matrices, so
    // inventing axes here makes the glove curl against the landmarks.
    // Palm normal from the live/bind hand basis. Used only for the wrist roll:
    // shortest-arc along the wrist→MCP axis leaves twist free, which is what
    // put the palm on the back of the hand.
    // Both sides of the basis come from the same construction so a hand at the
    // bind pose maps to identity. The bind side must sample the MCP joints
    // (BONE_*1), not the metacarpal stubs (BONE_*0): the stubs sit at 18-26 mm
    // from the wrist on a different ray than the live MCP joints at ~90 mm, and
    // mixing the two leaves a few degrees of standing roll offset.
    // `palm_plane_normal` orients itself out of the palm from the thumb, so
    // no chirality sign is applied here — one did, and it inverted the roll on
    // one hand, which spins the wrist the wrong way as it turns. Both `palm_bind`
    // and `palm_live` are the single pair computed above, shared with the
    // metacarpal stubs so the whole skeleton shares one basis.
    // Reference finger axis: middle MCP, so it matches the live basis axis and
    // is long enough to stay well conditioned when the hand tilts.
    let finger_bind = vsub(bind_pos[BONE_MIDDLE1], bind_pos[BONE_WRIST]);
    let finger_live = vsub(positions[BONE_MIDDLE1], positions[BONE_WRIST]);

    let mut frames = [[1.0f32; 4]; NUM_BONES];
    for i in 0..NUM_BONES {
        if i == BONE_WRIST {
            // Full 3-DOF wrist: map bind (finger, palm) → live (finger, palm).
            // Curl joints are untouched; this only rolls the glove so the
            // palm faces the same way as the landmarks. Degenerate live
            // basis (zero landmarks): finger-only delta, else identity.
            let delta = match (palm_bind, palm_live) {
                (Some(pb), Some(pl)) if vnorm(finger_live) > 1e-9 && vnorm(finger_bind) > 1e-9 => {
                    quat_from_basis(finger_bind, pb, finger_live, pl)
                }
                _ if vnorm(finger_live) > 1e-9 && vnorm(finger_bind) > 1e-9 => {
                quat_from_to(vnormalize(finger_bind), vnormalize(finger_live))
                }
                _ => [1.0, 0.0, 0.0, 0.0],
            };
            frames[i] = quat_mul(delta, bind_q[i]);
            continue;
        }
        match ORIENT_SEG[i] {
            Some((a, b)) => {
                let bd = vnormalize(vsub(bind_pos[b], bind_pos[a]));
                let ld = vnormalize(vsub(positions[b], positions[a]));
                // Compose each bone's live frame on top of its parent's live
                // frame instead of rotating the whole hand in model space.
                // The parent already carries the hand's roll, so this bone only
                // has to add its own flexion: the shortest arc between the bind
                // and live segment directions expressed in the parent frame.
                // That is a hinge, so it adds no twist about the bone axis.
                let parent = PARENT[i] as usize;
                let bp = quat_conj(bind_q[parent]);
                let flex = if vnorm(bd) > 1e-9 && vnorm(ld) > 1e-9 {
                    quat_from_to(
                        vnormalize(quat_rot_vec(bp, bd)),
                        vnormalize(quat_rot_vec(quat_conj(frames[parent]), ld)),
                    )
                } else {
                    // A collapsed segment has no direction to hinge about, so
                    // this bone adds nothing. Reusing another bone's rotation
                    // would double-apply a global delta or a neighbouring
                    // finger's flexion.
                    [1.0, 0.0, 0.0, 0.0]
                };
                frames[i] = quat_mul(quat_mul(frames[parent], flex), quat_mul(bp, bind_q[i]));
            }
            None => {
                // Tips and root carry no segment of their own: rigid with the
                // parent, so they inherit the parent's frame.
                frames[i] = if i == BONE_ROOT {
                    [1.0, 0.0, 0.0, 0.0]
                } else {
                    let parent = PARENT[i] as usize;
                    quat_mul(
                        frames[parent],
                        quat_mul(quat_conj(bind_q[parent]), bind_q[i]),
                    )
                };
            }
        }
    }

    // Aux bones (26-30) are Root children with no ORIENT_SEG of their own, so
    // the None branch above would hand every one of them the pinky's tip
    // frame. Each aux tracks its own finger's tip instead; bind quats for
    // aux/tip pairs are identical in the GLB, so sharing the tip's model-space
    // frame keeps orientation and position in sync.
    //
    // The aux node is a Root child whose bind origin sits on the DIP (see
    // finger_*_aux in vr_glove_*_model_slim.glb), not a phalanx in the chain.
    // It exists to be pushed out to the fingertip: SteamVR skins with
    // M @ IBM, so parking aux on its own bind origin makes that transform
    // exactly identity and the fingertip geometry it drives stops moving.
    const AUX_TIP: [(usize, usize); 5] = [
        (BONE_AUX_THUMB, BONE_THUMB3),
        (BONE_AUX_INDEX, BONE_INDEX4),
        (BONE_AUX_MIDDLE, BONE_MIDDLE4),
        (BONE_AUX_RING, BONE_RING4),
        (BONE_AUX_PINKY, BONE_PINKY4),
    ];
    for (aux, tip) in AUX_TIP {
        frames[aux] = frames[tip];
    }

    // Parent-relative transforms. Root carries the absolute wrist anchor
    // for the driver pose (position) with identity rotation; the driver
    // resets bone 0 to (0, identity) before pushing the skeleton.
    let mut bones = [BoneTransform::default(); NUM_BONES];
    for i in 0..NUM_BONES {
        let p = PARENT[i];
        if p < 0 {
            bones[i] = BoneTransform {
                position: [0.0; 3],
                orientation: [1.0, 0.0, 0.0, 0.0],
            };
        } else {
            let pu = p as usize;
            let rq = quat_conj(frames[pu]);
            bones[i] = BoneTransform {
                position: quat_rot_vec(rq, vsub(positions[i], positions[pu])),
                orientation: quat_mul(rq, frames[i]),
            };
        }
    }
    bones[BONE_ROOT].position = wrist_abs;
    // Orientations are continuous with the previous frame, which is what keeps a
    // roll from crossing a quaternion sign boundary. Done last, on the values
    // actually sent, because these parent-relative quats are what SteamVR
    // interpolates between.
    if let Some(prev) = prev_orientations {
        for i in 0..NUM_BONES {
            bones[i].orientation = match_sign_to_previous(bones[i].orientation, prev[i]);
        }
    }
    bones
}

/// Puts the root (device pose) into tracking space: rotates the absolute
/// wrist anchor and sets the root orientation to the composed head quat.
/// Bones 1..30 are parent-relative in the device frame and are left alone —
/// SteamVR applies the device pose above them, so pre-rotating them here
/// would apply the head transform twice.
/// Extra pitch on the hand device pose (degrees, + = up). Compensates for
/// the phone camera's downward tilt vs the user's gaze; raise if hands sit
/// low, lower/negative if they point at your face.
const HAND_PITCH_DEG: f32 = 0.0;

pub fn apply_head_pose(bones: &mut [BoneTransform; NUM_BONES], head_quat: [f32; 4]) {
    // Quaternion for rotation around X axis: (cos(θ/2), sin(θ/2), 0, 0).
    let pitch_rad = HAND_PITCH_DEG.to_radians();
    let half = pitch_rad * 0.5;
    let pitch_q = [half.cos(), half.sin(), 0.0, 0.0];
    // Compose: first pitch in camera space, then head rotation to tracking space.
    let q = quat_mul(head_quat, pitch_q);
    bones[BONE_ROOT].position = quat_rot_vec(q, bones[BONE_ROOT].position);
    bones[BONE_ROOT].orientation = q;
}

/// Quaternion conjugate (unit quats only).
fn quat_conj(q: [f32; 4]) -> [f32; 4] {
    [q[0], -q[1], -q[2], -q[3]]
}

/// Rotate a vector by a unit quaternion.
fn quat_rot_vec(q: [f32; 4], v: [f32; 3]) -> [f32; 3] {
    let u = [q[1], q[2], q[3]];
    let uv = vcross(u, v);
    let uuv = vcross(u, uv);
    [
        v[0] + 2.0 * (q[0] * uv[0] + uuv[0]),
        v[1] + 2.0 * (q[0] * uv[1] + uuv[1]),
        v[2] + 2.0 * (q[0] * uv[2] + uuv[2]),
    ]
}

/// Serialize bone transforms for UDP transmission to the driver.
/// Wire format: [tag=0x13][timestamp_ms u64 LE][hand_id u8][num_bones u8][bone0..boneN as 7×f32 LE]
pub fn serialize_bones_udp(
    hand_id: u8,
    timestamp_ms: u64,
    bones: &[BoneTransform; NUM_BONES],
) -> Vec<u8> {
    let mut buf = Vec::with_capacity(1 + 8 + 1 + 1 + NUM_BONES * 7 * 4);
    buf.push(0x13); // skeleton tag
    buf.extend_from_slice(&timestamp_ms.to_le_bytes());
    buf.push(hand_id);
    buf.push(NUM_BONES as u8);
    for b in bones {
        for v in &b.position {
            buf.extend_from_slice(&v.to_le_bytes());
        }
        for v in &b.orientation {
            buf.extend_from_slice(&v.to_le_bytes());
        }
    }
    buf
}

/// Deserialize bone transforms from a UDP skeleton packet (after tag is consumed).
pub fn deserialize_bones_udp(data: &[u8]) -> Option<(u8, u64, [BoneTransform; NUM_BONES])> {
    if data.len() < 10 {
        return None;
    }
    let timestamp_ms = u64::from_le_bytes(data[0..8].try_into().unwrap());
    let hand_id = data[8];
    let num_bones = data[9] as usize;
    if num_bones != NUM_BONES {
        return None;
    }
    let expected = 10 + NUM_BONES * 7 * 4;
    if data.len() < expected {
        return None;
    }
    let mut bones = [BoneTransform::default(); NUM_BONES];
    let mut off = 10;
    for i in 0..NUM_BONES {
        let p = [
            f32::from_le_bytes(data[off..off + 4].try_into().unwrap()),
            f32::from_le_bytes(data[off + 4..off + 8].try_into().unwrap()),
            f32::from_le_bytes(data[off + 8..off + 12].try_into().unwrap()),
        ];
        off += 12;
        let q = [
            f32::from_le_bytes(data[off..off + 4].try_into().unwrap()),
            f32::from_le_bytes(data[off + 4..off + 8].try_into().unwrap()),
            f32::from_le_bytes(data[off + 8..off + 12].try_into().unwrap()),
            f32::from_le_bytes(data[off + 12..off + 16].try_into().unwrap()),
        ];
        off += 16;
        bones[i] = BoneTransform {
            position: p,
            orientation: q,
        };
    }
    Some((hand_id, timestamp_ms, bones))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake_landmark(x: f32, y: f32, z: f32) -> Landmark {
        Landmark { x, y, z }
    }

    fn standard_hand() -> [Landmark; 21] {
        [
            fake_landmark(0.5, 0.8, 0.0),    // 0 WRIST
            fake_landmark(0.45, 0.7, 0.01),  // 1 THUMB_CMC
            fake_landmark(0.4, 0.6, 0.02),   // 2 THUMB_MCP
            fake_landmark(0.35, 0.55, 0.03), // 3 THUMB_IP
            fake_landmark(0.3, 0.5, 0.04),   // 4 THUMB_TIP
            fake_landmark(0.42, 0.55, 0.01), // 5 INDEX_MCP
            fake_landmark(0.42, 0.45, 0.01), // 6 INDEX_PIP
            fake_landmark(0.42, 0.38, 0.01), // 7 INDEX_DIP
            fake_landmark(0.42, 0.3, 0.01),  // 8 INDEX_TIP
            fake_landmark(0.48, 0.55, 0.01), // 9 MIDDLE_MCP
            fake_landmark(0.48, 0.44, 0.01), // 10 MIDDLE_PIP
            fake_landmark(0.48, 0.36, 0.01), // 11 MIDDLE_DIP
            fake_landmark(0.48, 0.28, 0.01), // 12 MIDDLE_TIP
            fake_landmark(0.54, 0.55, 0.01), // 13 RING_MCP
            fake_landmark(0.54, 0.45, 0.01), // 14 RING_PIP
            fake_landmark(0.54, 0.38, 0.01), // 15 RING_DIP
            fake_landmark(0.54, 0.3, 0.01),  // 16 RING_TIP
            fake_landmark(0.58, 0.58, 0.01), // 17 PINKY_MCP
            fake_landmark(0.58, 0.5, 0.01),  // 18 PINKY_PIP
            fake_landmark(0.58, 0.44, 0.01), // 19 PINKY_DIP
            fake_landmark(0.58, 0.38, 0.01), // 20 PINKY_TIP
        ]
    }

    #[test]
    fn root_and_wrist_are_both_at_wrist() {
        let lm = standard_hand();
        let bones = compute_bones(&lm);
        assert_eq!(bones[BONE_ROOT].x, lm[WRIST].x);
        assert_eq!(bones[BONE_ROOT].y, lm[WRIST].y);
        assert_eq!(bones[BONE_WRIST].x, lm[WRIST].x);
        assert_eq!(bones[BONE_WRIST].y, lm[WRIST].y);
    }

    #[test]
    fn thumb_bones_are_direct_copies() {
        let lm = standard_hand();
        let bones = compute_bones(&lm);
        assert_eq!(bones[BONE_THUMB0].x, lm[THUMB_CMC].x);
        assert_eq!(bones[BONE_THUMB1].x, lm[THUMB_MCP].x);
        assert_eq!(bones[BONE_THUMB2].x, lm[THUMB_IP].x);
        assert_eq!(bones[BONE_THUMB3].x, lm[THUMB_TIP].x);
    }

    #[test]
    fn mcp_pip_dip_tip_are_direct_copies() {
        let lm = standard_hand();
        let bones = compute_bones(&lm);
        // Index MCP, PIP, DIP, TIP
        assert_eq!(bones[BONE_INDEX1].x, lm[INDEX_MCP].x);
        assert_eq!(bones[BONE_INDEX2].x, lm[INDEX_PIP].x);
        assert_eq!(bones[BONE_INDEX3].x, lm[INDEX_DIP].x);
        assert_eq!(bones[BONE_INDEX4].x, lm[INDEX_TIP].x);
        // Middle
        assert_eq!(bones[BONE_MIDDLE1].x, lm[MIDDLE_MCP].x);
        assert_eq!(bones[BONE_MIDDLE2].x, lm[MIDDLE_PIP].x);
        assert_eq!(bones[BONE_MIDDLE3].x, lm[MIDDLE_DIP].x);
        assert_eq!(bones[BONE_MIDDLE4].x, lm[MIDDLE_TIP].x);
        // Ring
        assert_eq!(bones[BONE_RING1].x, lm[RING_MCP].x);
        assert_eq!(bones[BONE_RING2].x, lm[RING_PIP].x);
        assert_eq!(bones[BONE_RING3].x, lm[RING_DIP].x);
        assert_eq!(bones[BONE_RING4].x, lm[RING_TIP].x);
        // Pinky
        assert_eq!(bones[BONE_PINKY1].x, lm[PINKY_MCP].x);
        assert_eq!(bones[BONE_PINKY2].x, lm[PINKY_PIP].x);
        assert_eq!(bones[BONE_PINKY3].x, lm[PINKY_DIP].x);
        assert_eq!(bones[BONE_PINKY4].x, lm[PINKY_TIP].x);
    }

    #[test]
    fn metacarpals_are_between_wrist_and_mcp() {
        let lm = standard_hand();
        let bones = compute_bones(&lm);
        // Index metacarpal should be between wrist.x and INDEX_MCP.x
        let meta = bones[BONE_INDEX0];
        let w = lm[WRIST];
        let mcp = lm[INDEX_MCP];
        // X: meta should be closer to wrist than MCP is
        let meta_dist_from_wrist = (meta.x - w.x).abs();
        let mcp_dist_from_wrist = (mcp.x - w.x).abs();
        assert!(
            meta_dist_from_wrist < mcp_dist_from_wrist,
            "metacarpal should be between wrist and MCP"
        );
    }

    #[test]
    fn tip_is_beyond_dip() {
        let lm = standard_hand();
        let bones = compute_bones(&lm);
        assert!(
            norm(sub(bones[BONE_INDEX4], bones[BONE_INDEX3]))
                > norm(sub(lm[INDEX_TIP], lm[INDEX_DIP])) * 0.9,
            "index TIP bone should sit ~at the TIP landmark past the DIP"
        );
    }

    #[test]
    fn aux_bones_match_their_tips() {
        let lm = standard_hand();
        let bones = compute_bones(&lm);
        assert_eq!(bones[BONE_AUX_THUMB].x, bones[BONE_THUMB3].x);
        assert_eq!(bones[BONE_AUX_INDEX].x, bones[BONE_INDEX4].x);
        assert_eq!(bones[BONE_AUX_MIDDLE].x, bones[BONE_MIDDLE4].x);
        assert_eq!(bones[BONE_AUX_RING].x, bones[BONE_RING4].x);
        assert_eq!(bones[BONE_AUX_PINKY].x, bones[BONE_PINKY4].x);
    }

    #[test]
    fn original_landmarks_not_mutated() {
        let lm = standard_hand();
        let original = lm;
        let _ = compute_bones(&lm);
        for i in 0..21 {
            assert_eq!(lm[i].x, original[i].x);
            assert_eq!(lm[i].y, original[i].y);
            assert_eq!(lm[i].z, original[i].z);
        }
    }

    #[test]
    fn extra_bone_count_matches_constant() {
        assert_eq!(EXTRA_BONE_INDEXES.len(), 11);
    }

    /// Synthetic metric hand: wrist at origin, fingers along +Y (MediaPipe
    /// convention: +Y down, +Z away from camera), ~2x Valve size so the
    /// proportion scale has something to do.
    fn metric_hand() -> [Landmark; 21] {
        [
            fake_landmark(0.0, 0.0, 0.0),      // 0 WRIST
            fake_landmark(0.05, 0.03, 0.01),   // 1 THUMB_CMC
            fake_landmark(0.07, 0.05, 0.015),  // 2 THUMB_MCP
            fake_landmark(0.085, 0.07, 0.02),  // 3 THUMB_IP
            fake_landmark(0.10, 0.09, 0.025),  // 4 THUMB_TIP
            fake_landmark(0.04, 0.10, 0.01),   // 5 INDEX_MCP
            fake_landmark(0.04, 0.15, 0.012),  // 6 INDEX_PIP
            fake_landmark(0.04, 0.19, 0.014),  // 7 INDEX_DIP
            fake_landmark(0.04, 0.23, 0.016),  // 8 INDEX_TIP
            fake_landmark(0.0, 0.11, 0.01),    // 9 MIDDLE_MCP
            fake_landmark(0.0, 0.16, 0.012),   // 10 MIDDLE_PIP
            fake_landmark(0.0, 0.20, 0.014),   // 11 MIDDLE_DIP
            fake_landmark(0.0, 0.24, 0.016),   // 12 MIDDLE_TIP
            fake_landmark(-0.04, 0.10, 0.01),  // 13 RING_MCP
            fake_landmark(-0.04, 0.15, 0.012), // 14 RING_PIP
            fake_landmark(-0.04, 0.19, 0.014), // 15 RING_DIP
            fake_landmark(-0.04, 0.23, 0.016), // 16 RING_TIP
            fake_landmark(-0.07, 0.08, 0.01),  // 17 PINKY_MCP
            fake_landmark(-0.07, 0.12, 0.012), // 18 PINKY_PIP
            fake_landmark(-0.07, 0.15, 0.014), // 19 PINKY_DIP
            fake_landmark(-0.07, 0.18, 0.016), // 20 PINKY_TIP
        ]
    }

    /// Synthetic image hand: wrist centered, middle tip well above it so the
    /// apparent span is large (close hand).
    fn image_hand() -> [Landmark; 21] {
        let mut lm = [fake_landmark(0.5, 0.5, 0.0); 21];
        lm[0] = fake_landmark(0.5, 0.6, -0.02); // wrist
        lm[12] = fake_landmark(0.5, 0.3, -0.03); // middle tip
        lm
    }

    fn quat_len(q: [f32; 4]) -> f32 {
        (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]).sqrt()
    }

    /// Left-hand convention wrapper (no chirality flip).
    fn world_bones(img: &[Landmark; 21], wlm: &[Landmark; 21]) -> [BoneTransform; NUM_BONES] {
        compute_world_bones(img, wlm, false)
    }

    #[test]
    fn world_root_is_absolute_rest_are_local() {
        let bones = world_bones(&image_hand(), &metric_hand());
        // Bone 0 anchors the device pose: ~arm's length out, in front (-Z).
        let root = bones[BONE_ROOT].position;
        assert!(
            root[2] < -0.2 && root[2] > -1.3,
            "root z should be a sane depth, got {}",
            root[2]
        );
        // Bones 1..30 are wrist-relative: everything within a hand span.
        for i in 1..NUM_BONES {
            let p = bones[i].position;
            let d = (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]).sqrt();
            assert!(d < 0.4, "bone {i} should be wrist-relative, got dist {d}");
        }
    }

    #[test]
    fn world_metacarpals_match_valve_lengths() {
        // A hand already at the bind pose must put every stub back on its bind
        // joint, so both the stub→MCP length and its axis come from BIND_POS.
        // Valve's wrist→stub distances are a different, much shorter segment
        // (26 vs 74 mm on the index) and do not satisfy this.
        let pairs = [
            (BONE_INDEX0, BONE_INDEX1),
            (BONE_MIDDLE0, BONE_MIDDLE1),
            (BONE_RING0, BONE_RING1),
            (BONE_PINKY0, BONE_PINKY1),
        ];
        let wpos = compute_positions(&image_hand(), &bind_pose_landmarks(), false);
        for (stub, mcp) in pairs {
            let d = sub3(wpos[mcp], wpos[stub]);
            let len = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
            let exp = vnorm(sub3(BIND_POS_LEFT[mcp], BIND_POS_LEFT[stub]));
            assert!(
                (len - exp).abs() < 1e-6,
                "bone {stub}->{mcp} len {len}, bind length {exp}"
            );
            let axis = norm3(sub3(BIND_POS_LEFT[mcp], BIND_POS_LEFT[stub]));
            let dir = norm3(d);
            assert!(
                vdot(dir, axis) > 0.9999,
                "bone {stub}->{mcp} is {} deg off Valve's metacarpal axis",
                angle_deg(dir, axis)
            );
        }
    }

    /// The metacarpal stub must sit on Valve's metacarpal axis, not on the
    /// wrist→MCP ray. Placing it on the wrist ray leaves a few degrees of
    /// standing error in the stub→MCP segment, and every bone below it inherits
    /// that rotation — which shows up as a finger chain that twists as the MCP
    /// moves rather than splaying cleanly.
    #[test]
    fn metacarpal_stub_lies_on_the_bind_axis() {
        let img = image_hand();
        for is_right in [false, true] {
            let wlm = bind_pose_landmarks_for(is_right);
            let bones = if is_right {
                compute_world_bones(&img, &wlm, true)
            } else {
                world_bones(&img, &wlm)
            };
            let _ = &bones;
            let wpos = compute_positions(
                &image_hand(),
                &bind_pose_landmarks_for(is_right),
                is_right,
            );
            let bp = if is_right {
                &BIND_POS_RIGHT
            } else {
                &BIND_POS_LEFT
            };
            for (stub, mcp) in [
                (BONE_INDEX0, BONE_INDEX1),
                (BONE_MIDDLE0, BONE_MIDDLE1),
                (BONE_RING0, BONE_RING1),
                (BONE_PINKY0, BONE_PINKY1),
            ] {
                let live = norm3(sub3(wpos[mcp], wpos[stub]));
                let bind = norm3(sub3(bp[mcp], bp[stub]));
                let dot = live[0] * bind[0] + live[1] * bind[1] + live[2] * bind[2];
                assert!(
                    dot > 0.9999,
                    "right={is_right} bone {stub}: stub axis is {:.2} deg off \
                     Valve's metacarpal axis (dot={dot})",
                    angle_deg(live, bind)
                );
            }
        }
    }

    /// Isolates quat_from_basis: rotating only the live pair by R must
    /// left-multiply the resulting delta by exactly R (bind side untouched).
    #[test]
    fn basis_delta_follows_live_rotation() {
        // Finger axis and bind palm normal come from the bind tables rather than
        // literals, so this stays valid when the basis construction changes.
        let bp = &BIND_POS_LEFT;
        let fb = vsub(bp[BONE_MIDDLE1], bp[BONE_WRIST]);
        let pb = palm_plane_normal(bp).expect("bind palm ring");
        let fl = [0.0f32, -0.017826488, -0.0016205898];
        let pl = [-0.022628153f32, 0.09051256, -0.9956382];
        let half = 30.0f32.to_radians();
        let q_r = [half.cos(), 0.0, -half.sin(), 0.0]; // -60° about Y
        let d0 = quat_from_basis(fb, pb, fl, pl);
        let fl1 = quat_rot_vec(q_r, fl);
        let pl1 = quat_rot_vec(q_r, pl);
        let d1 = quat_from_basis(fb, pb, fl1, pl1);
        let want = quat_mul(q_r, d0);
        let dot = (want[0] * d1[0] + want[1] * d1[1] + want[2] * d1[2] + want[3] * d1[3]).abs();
        assert!(dot > 0.999, "basis delta not equivariant, dot={dot}");
    }

    /// Rigid spin of the whole hand about the finger axis must rotate every
    /// bone's skin frame (model frame ∘ conj(bind)) by exactly that spin.
    /// A single-vector arc delta is blind to roll about the segment axis.
    #[test]
    fn rigid_spin_rotates_every_skin_frame_with_the_hand() {
        // Bind landmarks, so every joint is a real hand joint and the spin is
        // an exact rigid rotation of the whole skeleton. Spin about the wrist,
        // which sits at the model origin, so the hand rotates in place.
        let base = bind_pose_landmarks();
        let pivot = base[WRIST];
        let (s, c) = (60.0f32.to_radians().sin(), 60.0f32.to_radians().cos());
        let spun = base.map(|l| {
            let (dx, dz) = (l.x - pivot.x, l.z - pivot.z);
            fake_landmark(
                pivot.x + c * dx - s * dz,
                l.y,
                pivot.z + s * dx + c * dz,
            )
        });
        let img = image_hand();
        let (_, rot_base) = compose_chain(&world_bones(&img, &base));
        let (_, rot_spun) = compose_chain(&world_bones(&img, &spun));
        // Landmarks spin +60° about MediaPipe +Y; the [x,-y,-z] axis flip maps that
        // to +60° about OpenVR -Y.
        let half = 30.0f32.to_radians();
        let q_spin = [half.cos(), 0.0, half.sin(), 0.0];
        let chain = [
            BONE_WRIST,
            BONE_INDEX0,
            BONE_INDEX1,
            BONE_INDEX2,
            BONE_INDEX3,
            BONE_INDEX4,
        ];
        let mut dots = Vec::new();
        for &i in &chain {
            let bind = BIND_QUAT_LEFT[i];
            let skin_base = quat_mul(rot_base[i], quat_conj(bind));
            let skin_spun = quat_mul(rot_spun[i], quat_conj(bind));
            let want = quat_mul(q_spin, skin_base);
            let dot = (want[0] * skin_spun[0]
                + want[1] * skin_spun[1]
                + want[2] * skin_spun[2]
                + want[3] * skin_spun[3])
                .abs();
            dots.push((i, dot));
        }
        eprintln!("skin-follow dots: {dots:?}");
        for (i, dot) in dots {
            assert!(
                dot > 0.99,
                "bone {i} skin does not follow a rigid spin, dot={dot}"
            );
        }
    }

    #[test]
    fn world_orientations_are_unit_quats() {
        let bones = world_bones(&image_hand(), &metric_hand());
        for i in 0..NUM_BONES {
            let l = quat_len(bones[i].orientation);
            assert!((l - 1.0).abs() < 0.01, "bone {i} quat len {l}");
        }
    }

    #[test]
    fn world_degenerate_input_has_no_nan() {
        let zero = [fake_landmark(0.0, 0.0, 0.0); 21];
        let bones = compute_world_bones(&zero, &zero, false);
        for i in 0..NUM_BONES {
            for v in bones[i].position {
                assert!(v.is_finite(), "bone {i} position not finite");
            }
            assert!((quat_len(bones[i].orientation) - 1.0).abs() < 0.01);
        }
    }

    /// Composing the parent-relative chain must recover the wrist-relative
    /// model joints (root and wrist both sit at the device origin).
    #[test]
    fn world_chain_reconstructs_model_joints() {
        let bones = world_bones(&image_hand(), &metric_hand());
        let (wpos, wrot) = compose_chain(&bones);
        // Wrist joint coincides with the device origin.
        let dw = (wpos[BONE_WRIST][0].powi(2)
            + wpos[BONE_WRIST][1].powi(2)
            + wpos[BONE_WRIST][2].powi(2))
        .sqrt();
        assert!(dw < 1e-5, "wrist should reconstruct to origin, got {dw}");
        // Middle fingertip reconstructs to ~the MediaPipe span (~0.24 m).
        let tip = wpos[BONE_MIDDLE4];
        let dt = (tip[0] * tip[0] + tip[1] * tip[1] + tip[2] * tip[2]).sqrt();
        assert!(
            (dt - 0.24).abs() < 0.05,
            "middle tip span {dt}, expected ~0.24"
        );
        // Composed orientations stay unit quaternions.
        for i in 1..NUM_BONES {
            assert!(
                (quat_len(wrot[i]) - 1.0).abs() < 0.01,
                "bone {i} composed quat"
            );
        }
    }

    fn add3(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
        [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
    }

    /// Compose a parent-relative chain into model-space joints, like SteamVR.
    fn compose_chain(
        bones: &[BoneTransform; NUM_BONES],
    ) -> ([[f32; 3]; NUM_BONES], [[f32; 4]; NUM_BONES]) {
        let id = [1.0f32, 0.0, 0.0, 0.0];
        let mut wpos = [[0.0f32; 3]; NUM_BONES];
        let mut wrot = [id; NUM_BONES];
        for i in 1..NUM_BONES {
            let p = PARENT[i] as usize;
            wrot[i] = quat_mul(wrot[p], bones[i].orientation);
            wpos[i] = add3(wpos[p], quat_rot_vec(wrot[p], bones[i].position));
        }
        (wpos, wrot)
    }

    /// Left-hand bone +X must point distal (toward the first child),
    /// matching the left vr_glove bind pose (children along +X).
    #[test]
    fn world_left_x_axis_points_distal() {
        let bones = world_bones(&image_hand(), &metric_hand());
        let (wpos, wrot) = compose_chain(&bones);
        for (bone, child) in [(BONE_INDEX1, BONE_INDEX2), (BONE_MIDDLE1, BONE_MIDDLE2)] {
            let x_axis = quat_rot_vec(wrot[bone], [1.0, 0.0, 0.0]);
            let to_child = norm3(sub3(wpos[child], wpos[bone]));
            let dot = x_axis[0] * to_child[0] + x_axis[1] * to_child[1] + x_axis[2] * to_child[2];
            assert!(
                dot > 0.9,
                "left bone {bone} +X should point at child, dot={dot}"
            );
        }
    }

/// Valve's two bind tables disagree on which way a bone's own +X runs: the
    /// left hand points it distal (toward the child) and the right hand
    /// proximal. Each hand's landmarks come from its own table, since the
    /// tables are not mirrors of one another.
    ///
    /// The threshold matches the reference data: Valve's own bind axes score
    /// 0.878/0.895 against their parent direction, so the bones are not exactly
    /// axial and asking for 0.9 would fail on a perfect reconstruction.
    #[test]
    fn bone_x_axis_follows_each_hands_bind_table() {
        let img = image_hand();
        for (is_right, toward_parent) in [(false, false), (true, true)] {
            let wlm = bind_pose_landmarks_for(is_right);
            let bones = if is_right {
                compute_world_bones(&img, &wlm, true)
            } else {
                world_bones(&img, &wlm)
            };
let (_, wrot) = compose_chain(&bones);
            // Measured against the bind table rather than the composed chain:
            // the metacarpal stub lives in model space, so composing the
            // parent-relative wire transforms back cannot reproduce it.
            let wpos = compute_positions(&img, &wlm, is_right);
            let bp = if is_right {
                &BIND_POS_RIGHT
            } else {
                &BIND_POS_LEFT
            };
            let bq = if is_right {
                &BIND_QUAT_RIGHT
            } else {
                &BIND_QUAT_LEFT
            };
            for (bone, parent, child) in [
                (BONE_INDEX1, BONE_INDEX0, BONE_INDEX2),
                (BONE_MIDDLE1, BONE_MIDDLE0, BONE_MIDDLE2),
            ] {
                let x_axis = quat_rot_vec(wrot[bone], [1.0, 0.0, 0.0]);
                let target = if toward_parent { parent } else { child };
                let to = norm3(sub3(bp[target], bp[bone]));
                let dot = x_axis[0] * to[0] + x_axis[1] * to[1] + x_axis[2] * to[2];
                let reference = vdot(quat_rot_vec(bq[bone], [1.0, 0.0, 0.0]), to);
                assert!(
                    dot >= reference - 1e-3,
                    "right={is_right} bone {bone} +X should reach its bind \
                     orientation (got {dot:.4}, bind {reference:.4})"
                );
            }
            // Positions are never mirrored: each finger's reconstructed joints must land
            // on that hand's own bind joints, tip included.
            for (tip_bone, bind_tip) in [
                (BONE_MIDDLE4, BONE_MIDDLE4),
                (BONE_INDEX4, BONE_INDEX4),
            ] {
                let d = sub3(wpos[tip_bone], wpos[BONE_WRIST]);
                let want = sub3(bp[bind_tip], bp[BONE_WRIST]);
                let err = sub3(d, want);
                let e = (err[0] * err[0] + err[1] * err[1] + err[2] * err[2]).sqrt();
                assert!(
                    e < 0.002,
                    "right={is_right} bone {tip_bone} tip is {e:.4} m from its bind position"
                );
            }
        }
    }

    fn sub3(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
        [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
    }

    fn norm3(v: [f32; 3]) -> [f32; 3] {
        let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
        if l < 1e-9 {
            return [1.0, 0.0, 0.0];
        }
        [v[0] / l, v[1] / l, v[2] / l]
    }

    /// Curling a finger toward the camera (palm facing the camera) must move
    /// the composed fingertip toward the palm side (+Z in OpenVR model space,
    /// toward the user). MediaPipe image z: smaller = closer to camera.
    /// Orientation must keep mapping the index bind segment onto the live
    /// segment so SteamVR's IBM skins the curl the same way as hand_tracker.
    #[test]
    fn world_curl_toward_camera_flexes_toward_palm() {
        let mut wlm = [fake_landmark(0.0, 0.0, 0.0); 21];
        wlm[WRIST] = fake_landmark(0.0, 0.0, 0.0);
        wlm[INDEX_MCP] = fake_landmark(0.02, -0.10, -0.005);
        wlm[INDEX_PIP] = fake_landmark(0.02, -0.15, -0.005);
        wlm[INDEX_DIP] = fake_landmark(0.02, -0.19, -0.005);
        wlm[MIDDLE_MCP] = fake_landmark(-0.01, -0.11, -0.005);
        let img = [fake_landmark(0.5, 0.5, 0.0); 21];
        let mut curled_img = img;
        curled_img[INDEX_TIP].z = -0.03;
        let mut straight = wlm;
        straight[INDEX_TIP] = fake_landmark(0.02, -0.23, -0.005);
        let mut curled = wlm;
        curled[INDEX_TIP] = fake_landmark(0.02, -0.205, -0.025);
        let straight_bones = compute_world_bones(&img, &straight, false);
        let curled_bones = compute_world_bones(&curled_img, &curled, false);
        let (pos_straight, rot_straight) = compose_chain(&straight_bones);
        let (pos_curled, _) = compose_chain(&curled_bones);
        assert!(
            pos_curled[BONE_INDEX4][2] > pos_straight[BONE_INDEX4][2],
            "curled tip z {} should be palm-side of straight tip z {}",
            pos_curled[BONE_INDEX4][2],
            pos_straight[BONE_INDEX4][2]
        );
        assert_bind_delta_maps_segments(&straight_bones, false, "straight");
        assert_bind_delta_maps_segments(&curled_bones, false, "curled");
        let _ = rot_straight;
    }

    /// Depth: MediaPipe smaller z = closer to camera; after [x,-y,-z] that
    /// becomes larger OpenVR z (nearer the user).
    #[test]
    fn world_depth_order_matches_mediapipe() {
        let mut wlm = [fake_landmark(0.0, 0.0, 0.0); 21];
        // The index chain laid straight along the depth axis at Valve's own bone
        // lengths, so normalize_segments is a no-op and the composed depth has
        // to come out at exactly the landmarks' depth.
        let (l0, l1, l2) = (
            vnorm(vsub(BIND_POS_LEFT[BONE_INDEX1], BIND_POS_LEFT[BONE_INDEX0])) * 0.0
                + vnorm(vsub(BIND_POS_LEFT[BONE_INDEX2], BIND_POS_LEFT[BONE_INDEX1])),
            vnorm(vsub(BIND_POS_LEFT[BONE_INDEX3], BIND_POS_LEFT[BONE_INDEX2])),
            vnorm(vsub(BIND_POS_LEFT[BONE_INDEX4], BIND_POS_LEFT[BONE_INDEX3])),
        );
        wlm[INDEX_PIP] = fake_landmark(0.0, 0.0, l0);
        wlm[INDEX_DIP] = fake_landmark(0.0, 0.0, l0 + l1);
        wlm[INDEX_TIP] = fake_landmark(0.0, 0.0, l0 + l1 + l2);
        let bones = world_bones(&image_hand(), &wlm);
        let (wpos, _) = compose_chain(&bones);
        let z = wpos[BONE_INDEX4][2];
        assert!(
            (z + l0 + l1 + l2).abs() < 1e-5,
            "tip depth {z}, expected ~{}",
            -(l0 + l1 + l2)
        );
    }

    /// Only `image_landmarks[0].x/.y` reach the skeleton (the wrist anchor);
    /// their z is ignored entirely. `hand_world_landmarks` z is the channel
    /// that carries finger curl, and on the distal phalanges it is an order
    /// of magnitude stronger than the image z — substituting the latter
    /// rotates a curled fingertip tens of degrees off its true direction.
    #[test]
    fn image_z_cannot_reach_the_bone_chain() {
        let img = image_hand();
        let wlm = metric_hand();
        let base = compute_world_bones(&img, &wlm, false);
        let mut skewed = img;
        for i in 0..21 {
            skewed[i].z = if i % 2 == 0 { 0.3 } else { -0.3 };
        }
        let other = compute_world_bones(&skewed, &wlm, false);
        for i in 0..NUM_BONES {
            for k in 0..3 {
                assert_eq!(base[i].position[k], other[i].position[k], "bone {i} pos[{k}]");
            }
            for k in 0..4 {
                assert_eq!(base[i].orientation[k], other[i].orientation[k], "bone {i} quat[{k}]");
            }
        }
    }

    /// The distal segment's depth must follow the world landmarks, so bending
    /// the fingertip toward the camera swings the composed tip with it.
    /// MediaPipe z grows away from the camera, so "toward" is a decrease.
    #[test]
    fn world_z_drives_the_distal_tip_swing() {
        let img = image_hand();
        // Asserted as a monotone response rather than a fixed depth: the tip now
        // sits at Valve's bone length, so the raw z step reaches it through the
        // normalization gain instead of unscaled. Direction and ordering are
        // what this test is about, and those are what a fixed number would miss.
        let swing_of = |dz: f32| {
            let mut wlm = metric_hand();
            wlm[INDEX_TIP].z -= dz;
            let (pos, _) = compose_chain(&compute_world_bones(&img, &wlm, false));
            let (base, _) = compose_chain(&compute_world_bones(&img, &metric_hand(), false));
            pos[BONE_INDEX4][2] - base[BONE_INDEX4][2]
        };
        let small = swing_of(0.01);
        let large = swing_of(0.03);
        assert!(small > 0.0, "tip must track world z toward the user, got {small}");
        assert!(
            large > small,
            "a larger world-z step gave a smaller swing ({large} vs {small}); \
             the response has to be monotone"
        );
    }

    /// Model-space orientation of every ORIENT_SEG bone must take that
    /// hand's Valve bind segment onto the live segment (hand_tracker's
    /// D @ bind). Bone orientations are local→model, so the bind-model
    /// segment is rotated into the bone frame first (conj(bind_q)).
    fn assert_bind_delta_maps_segments(
        bones: &[BoneTransform; NUM_BONES],
        is_right: bool,
        label: &str,
    ) {
        let bp = if is_right {
            &BIND_POS_RIGHT
        } else {
            &BIND_POS_LEFT
        };
        let bq = if is_right {
            &BIND_QUAT_RIGHT
        } else {
            &BIND_QUAT_LEFT
        };
        let (wpos, wrot) = compose_chain(bones);
        for i in 0..NUM_BONES {
            let Some((a, b)) = ORIENT_SEG[i] else {
                continue;
            };
            let bd_raw = sub3(bp[b], bp[a]);
            let ld_raw = sub3(wpos[b], wpos[a]);
            if vnorm(bd_raw) < 1e-6 || vnorm(ld_raw) < 1e-6 {
                continue;
            }
            let bd = norm3(bd_raw);
            let ld = norm3(ld_raw);
            let local = norm3(quat_rot_vec(quat_conj(bq[i]), bd));
            let mapped = norm3(quat_rot_vec(wrot[i], local));
            let dot = mapped[0] * ld[0] + mapped[1] * ld[1] + mapped[2] * ld[2];
            assert!(
                dot > 0.95,
                "{label} hand right={is_right} bone {i}: bind dir mapped to {mapped:?}, live {ld:?}, dot={dot}"
            );
        }
    }

    /// Wrist frame maps the bind palm normal onto the live one (palm side
    /// matches landmarks; fingers still follow their segments).
    #[test]
    fn world_wrist_palm_matches_live_normal() {
        for is_right in [false, true] {
            let bones = compute_world_bones(&image_hand(), &metric_hand(), is_right);
            let (wpos, wrot) = compose_chain(&bones);
            let sign = if is_right { -1.0 } else { 1.0 };
            let signed = |n: [f32; 3]| [n[0] * sign, n[1] * sign, n[2] * sign];
            let palm_live = signed(
                palm_plane_normal(&wpos).expect("live palm ring is non-degenerate"),
            );
            // Bind palm in bone-local, pushed through the wrist orientation.
            let bp = if is_right {
                &BIND_POS_RIGHT
            } else {
                &BIND_POS_LEFT
            };
            let bq = if is_right {
                &BIND_QUAT_RIGHT
            } else {
                &BIND_QUAT_LEFT
            };
            let palm_bind = signed(palm_plane_normal(bp).expect("bind palm ring"));
            let local = norm3(quat_rot_vec(quat_conj(bq[BONE_WRIST]), palm_bind));
            let mapped = norm3(quat_rot_vec(wrot[BONE_WRIST], local));
            let dot =
                mapped[0] * palm_live[0] + mapped[1] * palm_live[1] + mapped[2] * palm_live[2];
            assert!(
                dot > 0.9,
                "right={is_right} wrist palm dot={dot} mapped={mapped:?} live={palm_live:?}"
            );
        }
    }

    #[test]
    fn world_frames_map_bind_onto_live_both_hands() {
        let bones_l = world_bones(&image_hand(), &metric_hand());
        let bones_r = compute_world_bones(&image_hand(), &metric_hand(), true);
        assert_bind_delta_maps_segments(&bones_l, false, "metric");
        assert_bind_delta_maps_segments(&bones_r, true, "metric");
    }

    /// Each aux bone's composed transform must equal its finger's tip. Aux
    /// sits outside the phalanx chain (Root child, bind origin on the DIP),
    /// so it only reaches the fingertip if fed the tip; on its own bind
    /// origin its M @ IBM is identity and the fingertip geometry is frozen.
    #[test]
    fn aux_frames_match_their_tips() {
        let pairs = [
            (BONE_AUX_THUMB, BONE_THUMB3),
            (BONE_AUX_INDEX, BONE_INDEX4),
            (BONE_AUX_MIDDLE, BONE_MIDDLE4),
            (BONE_AUX_RING, BONE_RING4),
            (BONE_AUX_PINKY, BONE_PINKY4),
        ];
        for is_right in [false, true] {
            let bones = compute_world_bones(&image_hand(), &metric_hand(), is_right);
            let (wpos, wrot) = compose_chain(&bones);
            for (aux, tip) in pairs {
                for k in 0..3 {
                    let d = (wpos[aux][k] - wpos[tip][k]).abs();
                    assert!(d < 1e-4, "right={is_right} aux {aux} vs tip {tip} pos[{k}] diff {d}");
                }
                for k in 0..4 {
                    let d = (wrot[aux][k] - wrot[tip][k]).abs();
                    assert!(d < 1e-4, "right={is_right} aux {aux} vs tip {tip} quat[{k}] diff {d}");
                }
            }
        }
    }

    /// Each aux must sit off its own bind origin. Valve parents the aux node
    /// to Root with its bind position on the finger's DIP, so a live aux
    /// parked on the DIP cancels that joint's skinning delta (M @ IBM == I)
    /// and the fingertip geometry stops following the hand. Bend only the
    /// tip: the DIP holds still, so an aux that travels with the tip is
    /// reaching past the knuckle, and one parked on the DIP travels not at
    /// all.
    #[test]
    fn aux_leaves_its_bind_origin_when_the_finger_curls() {
        let mut wlm = metric_hand();
        for tip in [INDEX_TIP, MIDDLE_TIP, RING_TIP, PINKY_TIP] {
            wlm[tip].z -= 0.03;
        }
        let (sp, _) = compose_chain(&compute_world_bones(&image_hand(), &metric_hand(), false));
        let (cp, _) = compose_chain(&compute_world_bones(&image_hand(), &wlm, false));
        for (aux, tip, dip) in [
            (BONE_AUX_INDEX, BONE_INDEX4, BONE_INDEX3),
            (BONE_AUX_MIDDLE, BONE_MIDDLE4, BONE_MIDDLE3),
            (BONE_AUX_RING, BONE_RING4, BONE_RING3),
            (BONE_AUX_PINKY, BONE_PINKY4, BONE_PINKY3),
        ] {
            let aux_travel = vnorm(vsub(cp[aux], sp[aux]));
            let dip_travel = vnorm(vsub(cp[dip], sp[dip]));
            let tip_travel = vnorm(vsub(cp[tip], sp[tip]));
            assert!(dip_travel < 1e-4, "DIP {dip} should hold still, got {dip_travel}");
            assert!(
                (aux_travel - tip_travel).abs() < 1e-4,
                "aux {aux} travel {aux_travel} should follow tip {tip} travel {tip_travel}"
            );
            assert!(
                aux_travel > dip_travel + 0.01,
                "aux {aux} travel {aux_travel} should clear its bind origin on the DIP"
            );
        }
    }

    /// apply_head_pose with identity head quat applies only the pitch
    /// correction (startup path, no head rotation yet).
    #[test]
    fn head_pose_identity_applies_pitch_only() {
        let pitch_rad = HAND_PITCH_DEG.to_radians();
        let half = pitch_rad * 0.5;
        let pitch_q = [half.cos(), half.sin(), 0.0, 0.0];

        let mut bones = world_bones(&image_hand(), &metric_hand());
        let before = bones[BONE_ROOT].position;
        apply_head_pose(&mut bones, [1.0, 0.0, 0.0, 0.0]);
        // Position is rotated by the pitch quaternion
        let expected = quat_rot_vec(pitch_q, before);
        assert!(
            (bones[BONE_ROOT].position[0] - expected[0]).abs() < 1e-5
                && (bones[BONE_ROOT].position[1] - expected[1]).abs() < 1e-5
                && (bones[BONE_ROOT].position[2] - expected[2]).abs() < 1e-5,
            "root position should be pitch-rotated"
        );
        // Orientation is the pitch quaternion
        assert!(
            (bones[BONE_ROOT].orientation[0] - pitch_q[0]).abs() < 1e-5
                && (bones[BONE_ROOT].orientation[1] - pitch_q[1]).abs() < 1e-5,
            "root orientation should be the pitch rotation"
        );
    }

    /// apply_head_pose composes head rotation with pitch correction: the
    /// combined rotation is applied to the skeleton.
    #[test]
    fn head_pose_rotates_skeleton_into_tracking_space() {
        let pitch_rad = HAND_PITCH_DEG.to_radians();
        let half = pitch_rad * 0.5;
        let pitch_q = [half.cos(), half.sin(), 0.0, 0.0];

        let mut bones = world_bones(&image_hand(), &metric_hand());
        let before_root = bones[BONE_ROOT].position;
        // 90° about +Y: [w,x,y,z] = [cos45, 0, sin45, 0].
        let q90y = [0.7071068, 0.0, 0.7071068, 0.0];
        apply_head_pose(&mut bones, q90y);
        // Combined rotation = head_quat * pitch_q
        let combined = quat_mul(q90y, pitch_q);
        // Position rotated by combined
        let expected = quat_rot_vec(combined, before_root);
        assert!(
            (bones[BONE_ROOT].position[0] - expected[0]).abs() < 1e-5
                && (bones[BONE_ROOT].position[1] - expected[1]).abs() < 1e-5
                && (bones[BONE_ROOT].position[2] - expected[2]).abs() < 1e-5,
            "root position should be rotated by combined quat"
        );
        // Orientation is the combined rotation
        assert!(
            (bones[BONE_ROOT].orientation[0] - combined[0]).abs() < 1e-5
                && (bones[BONE_ROOT].orientation[1] - combined[1]).abs() < 1e-5
                && (bones[BONE_ROOT].orientation[2] - combined[2]).abs() < 1e-5
                && (bones[BONE_ROOT].orientation[3] - combined[3]).abs() < 1e-5,
            "root orientation should be the combined rotation"
        );
    }

    /// The composed wrist orientation must take the bind wrist→middle-MCP
    /// segment onto the live one. That segment is the basis reference axis, so
    /// it also pins the palm-roll sign: getting it backwards spins the whole
    /// hand the wrong way as the wrist turns.
    #[test]
    fn wrist_follows_palm_rotation() {
        let bones = world_bones(&image_hand(), &metric_hand());
        assert_bind_delta_maps_segments(&bones, false, "metric");
        let (wpos, wrot) = compose_chain(&bones);
        let bd = norm3(sub3(BIND_POS_LEFT[BONE_MIDDLE1], BIND_POS_LEFT[BONE_WRIST]));
        let ld = norm3(sub3(wpos[BONE_MIDDLE1], wpos[BONE_WRIST]));
        let local = norm3(quat_rot_vec(quat_conj(BIND_QUAT_LEFT[BONE_WRIST]), bd));
        let mapped = norm3(quat_rot_vec(wrot[BONE_WRIST], local));
        let dot = mapped[0] * ld[0] + mapped[1] * ld[1] + mapped[2] * ld[2];
        assert!(
            dot > 0.95,
            "wrist maps bind→live, got {mapped:?} vs {ld:?} dot={dot}"
        );
    }

    /// apply_head_pose transforms only the root (absolute device pose);
    /// parent-relative bones stay in the device frame.
    #[test]
    fn head_pose_leaves_parent_relative_bones_untouched() {
        let mut bones = world_bones(&image_hand(), &metric_hand());
        let before_pos = bones[BONE_WRIST].position;
        let before_ori = bones[BONE_WRIST].orientation;
        let before_finger = bones[BONE_INDEX0].orientation;
        apply_head_pose(&mut bones, [0.7071068, 0.0, 0.7071068, 0.0]);
        assert_eq!(bones[BONE_WRIST].position, before_pos);
        assert_eq!(bones[BONE_WRIST].orientation, before_ori);
        assert_eq!(bones[BONE_INDEX0].orientation, before_finger);
    }

    /// A zero palm override falls back to the position-derived normal.
    #[test]
    fn degenerate_palm_override_falls_back_to_positions() {
        let img = image_hand();
        let wlm = metric_hand();
        let a = compute_world_bones(&img, &wlm, false);
        let b = compute_world_bones_with_palm(&img, &wlm, false, Some([0.0, 0.0, 0.0]), None);
        for i in 0..NUM_BONES {
            for k in 0..3 {
                assert!((a[i].position[k] - b[i].position[k]).abs() < 1e-6);
            }
            for k in 0..4 {
                assert!((a[i].orientation[k] - b[i].orientation[k]).abs() < 1e-6);
            }
        }
    }

    /// A valid palm override steers the wrist frame away from the raw normal.
    #[test]
    fn valid_palm_override_steers_wrist_frame() {
        let img = image_hand();
        let wlm = metric_hand();
        let a = compute_world_bones(&img, &wlm, false);
        let b = compute_world_bones_with_palm(&img, &wlm, false, Some([1.0, 0.0, 0.0]), None);
        let mut diff = 0.0f32;
        for k in 0..4 {
            diff = diff.max((a[BONE_WRIST].orientation[k] - b[BONE_WRIST].orientation[k]).abs());
        }
        assert!(
            diff > 1e-3,
            "override should move the wrist frame, diff={diff}"
        );
    }

    /// Feeding the caller-supplied normal must reproduce the position-derived
    /// fallback exactly, so the wire override and the offline path cannot
    /// disagree on wrist roll. Both sides build the normal the same way, which
    /// is what makes this an identity check rather than a tolerance check.
    #[test]
    fn palm_override_preserves_left_and_right_wrist_frames() {
        let img = image_hand();
        let wlm = metric_hand();
        let mut live = [[0.0f32; 3]; NUM_BONES];
        live[BONE_WRIST] = [
            wlm[WRIST].x,
            wlm[WRIST].y,
            wlm[WRIST].z,
        ];
        for (bone, lm) in [
            (BONE_INDEX1, INDEX_MCP),
            (BONE_MIDDLE1, MIDDLE_MCP),
            (BONE_RING1, RING_MCP),
            (BONE_PINKY1, PINKY_MCP),
        ] {
            live[bone] = [wlm[lm].x, wlm[lm].y, wlm[lm].z];
        }
        let palm = palm_plane_normal(&live).expect("palm ring is non-degenerate");
        for is_right in [false, true] {
            let fallback = compute_world_bones(&img, &wlm, is_right);
            // The override arrives in MediaPipe axes, which is the same basis
            // these world landmarks are in before the [x,-y,-z] conversion.
            let overridden = compute_world_bones_with_palm(&img, &wlm, is_right, Some(palm), None);
            for k in 0..4 {
                let diff = (fallback[BONE_WRIST].orientation[k]
                    - overridden[BONE_WRIST].orientation[k])
                    .abs();
                assert!(diff < 1e-5, "right={is_right} wrist quat[{k}] diff={diff}");
            }
        }
    }

    /// Each hand's composed frames must track *that* hand's Valve bind
    /// (left vs right tables differ — chirality selects the mirror).
    #[test]
    fn world_palm_normal_flips_with_chirality() {
        let left = world_bones(&image_hand(), &metric_hand());
        let right = compute_world_bones(&image_hand(), &metric_hand(), true);
        assert_bind_delta_maps_segments(&left, false, "chirality");
        assert_bind_delta_maps_segments(&right, true, "chirality");
        // Same landmarks, mirrored binds → different local orientations.
        let (_, wl) = compose_chain(&left);
        let (_, wr) = compose_chain(&right);
        let mut max_diff = 0.0f32;
        for bone in [BONE_INDEX0, BONE_MIDDLE0, BONE_WRIST] {
            for k in 0..4 {
                max_diff = max_diff.max((wl[bone][k] - wr[bone][k]).abs());
            }
        }
        assert!(
            max_diff > 0.1,
            "left/right binds should differ, max component diff {max_diff}"
        );
    }

    /// Live capture of one flat, uncurled left hand (%TEMP%/cbpp/hand_frame.txt
    /// seq=4932, 2026-09-28). Both landmark sets below are the model's output
    /// for the same frame, so any flexion the world set reports at a joint is
    /// depth the image set disagrees with.
    fn captured_world() -> [Landmark; 21] {
        [
            fake_landmark(-0.0111, 0.0814, -0.0294),
            fake_landmark(-0.0385, 0.0539, -0.0279),
            fake_landmark(-0.0572, 0.0314, -0.0159),
            fake_landmark(-0.0806, 0.0057, 0.0002),
            fake_landmark(-0.0992, -0.0148, 0.0176),
            fake_landmark(-0.0293, -0.0037, 0.0066),
            fake_landmark(-0.0261, -0.0322, 0.0068),
            fake_landmark(-0.0293, -0.0527, 0.0003),
            fake_landmark(-0.0340, -0.0638, -0.0247),
            fake_landmark(-0.0036, -0.0036, 0.0075),
            fake_landmark(0.0026, -0.0368, -0.0003),
            fake_landmark(0.0009, -0.0540, -0.0182),
            fake_landmark(-0.0026, -0.0701, -0.0370),
            fake_landmark(0.0191, 0.0011, -0.0034),
            fake_landmark(0.0267, -0.0253, -0.0087),
            fake_landmark(0.0279, -0.0421, -0.0216),
            fake_landmark(0.0266, -0.0577, -0.0377),
            fake_landmark(0.0300, 0.0163, -0.0160),
            fake_landmark(0.0477, -0.0010, -0.0124),
            fake_landmark(0.0589, -0.0172, -0.0197),
            fake_landmark(0.0638, -0.0318, -0.0269),
        ]
    }

    /// Same frame, image-landmark z only: the model's other depth estimate,
    /// in wrist-relative image-width units.
    fn captured_image_z() -> [f32; 21] {
        [
            0.0000, -0.0265, -0.0392, -0.0528, -0.0654, //
            0.0033, -0.0098, -0.0232, -0.0331, //
            -0.0003, -0.0080, -0.0157, -0.0211, //
            -0.0106, -0.0263, -0.0329, -0.0358, //
            -0.0246, -0.0395, -0.0426, -0.0428,
        ]
    }

    /// Per finger: landmark MCP/PIP/DIP/TIP then the matching bone indices.
    const CAPTURED_CHAINS: [(usize, usize, usize, usize, usize, usize, usize, usize); 4] = [
        (INDEX_MCP, INDEX_PIP, INDEX_DIP, INDEX_TIP,
         BONE_INDEX1, BONE_INDEX2, BONE_INDEX3, BONE_INDEX4),
        (MIDDLE_MCP, MIDDLE_PIP, MIDDLE_DIP, MIDDLE_TIP,
         BONE_MIDDLE1, BONE_MIDDLE2, BONE_MIDDLE3, BONE_MIDDLE4),
        (RING_MCP, RING_PIP, RING_DIP, RING_TIP,
         BONE_RING1, BONE_RING2, BONE_RING3, BONE_RING4),
        (PINKY_MCP, PINKY_PIP, PINKY_DIP, PINKY_TIP,
         BONE_PINKY1, BONE_PINKY2, BONE_PINKY3, BONE_PINKY4),
    ];

    fn seg3(lm: &[Landmark; 21], a: usize, b: usize) -> [f32; 3] {
        [lm[b].x - lm[a].x, lm[b].y - lm[a].y, lm[b].z - lm[a].z]
    }


    fn angle_deg(a: [f32; 3], b: [f32; 3]) -> f32 {
        let (la, lb) = (vnorm(a), vnorm(b));
        if la < 1e-9 || lb < 1e-9 {
            return 0.0;
        }
        (vdot(a, b) / (la * lb)).clamp(-1.0, 1.0).acos().to_degrees()
    }

    /// Every phalange must reach the skeleton at Valve's own bone length. On a real
    /// capture MediaPipe's phalanges arrive at 0.55-0.77x that, and SteamVR
    /// skins the glove with Valve's inverse-bind matrices, so a short bone drags
    /// the mesh and sweeps a shorter arc for the same joint angle — a pinky
    /// abduction of 30 deg moves the tip 21 mm at capture scale against 33 mm at
    /// bind scale, which is the fingers refusing to spread like a real hand.
    #[test]
    fn phalanges_arrive_at_valve_bone_lengths() {
        let lm = captured_world();
        let img = image_hand();
        for is_right in [false, true] {
            let (wp, _) = compose_chain(&compute_world_bones(&img, &lm, is_right));
            let bind = if is_right {
                &BIND_POS_RIGHT
            } else {
                &BIND_POS_LEFT
            };
            for i in 2..NUM_BONES {
                if METACARPAL_BONES.contains(&i) {
                    continue;
                }
                let Some((a, b)) = ORIENT_SEG[i] else { continue };
                let got = vnorm(vsub(wp[b], wp[a]));
                let want = vnorm(vsub(bind[b], bind[a]));
                assert!(
                    (got - want).abs() < 1e-5,
                    "right={is_right} bone {a}->{b} is {:.4} m, Valve's is {want:.4} m",
                    got
                );
            }
        }
    }

    /// normalize_segments rescales every PARENT edge to its Valve bind length
    /// along the live direction, so it must hand the landmark joint angles
    /// back untouched. A finger that curls while the hand is flat therefore
    /// comes from the landmarks, not from this function — on the captured
    /// hand the composed chain matches the landmarks at every joint.
    #[test]
    fn normalize_preserves_every_landmark_joint_angle() {
        let lm = captured_world();
        let img = [fake_landmark(0.5, 0.55, 0.0); 21];
        let (wp, _) = compose_chain(&compute_world_bones_with_palm(&img, &lm, false, None, None));
        for (lm_mcp, lm_pip, lm_dip, lm_tip, b1, b2, b3, b4) in CAPTURED_CHAINS {
            for (la, lb, lc, ba, bb, bc) in [
                (lm_mcp, lm_pip, lm_dip, b1, b2, b3),
                (lm_pip, lm_dip, lm_tip, b2, b3, b4),
            ] {
                let landmark = angle_deg(seg3(&lm, la, lb), seg3(&lm, lb, lc));
                let composed = angle_deg(
                    [wp[bb][0] - wp[ba][0], wp[bb][1] - wp[ba][1], wp[bb][2] - wp[ba][2]],
                    [wp[bc][0] - wp[bb][0], wp[bc][1] - wp[bb][1], wp[bc][2] - wp[bb][2]],
                );
                assert!(
                    (composed - landmark).abs() < 0.5,
                    "bones {ba}->{bb}->{bc}: composed {composed:.1} deg, \
                     landmarks say {landmark:.1} deg"
                );
            }
        }
    }

    /// On the flat capture the image-space depth steps down each finger at a
    /// near-constant rate while the world-space depth does not; the surplus in
    /// the last hop is the curl the rig renders on a hand that is not closing.
    /// Reported rather than asserted: this is the measurement a distal-segment
    /// depth fix is tuned against.
    #[test]
    fn report_capture_depth_rate_disagreement() {
        let lm = captured_world();
        let iz = captured_image_z();
        for (i, &(mcp, pip, dip, tip, ..)) in CAPTURED_CHAINS.iter().enumerate() {
            let name = ["index", "middle", "ring", "pinky"][i];
            let ids = [mcp, pip, dip, tip];
            let w: Vec<f32> = (0..3).map(|k| (lm[ids[k + 1]].z - lm[ids[k]].z) * 1000.0).collect();
            let i: Vec<f32> = (0..3).map(|k| iz[ids[k + 1]] - iz[ids[k]]).collect();
            eprintln!(
                "{name:6} world dz(mm)=[{:.1},{:.1},{:.1}] image dz=[{:.4},{:.4},{:.4}] \
                 flexion PIP={:.1} DIP={:.1} deg",
                w[0], w[1], w[2], i[0], i[1], i[2],
                angle_deg(seg3(&lm, mcp, pip), seg3(&lm, pip, dip)),
                angle_deg(seg3(&lm, pip, dip), seg3(&lm, dip, tip)),
            );
        }
    }

    /// Landmarks rebuilt from Valve's bind pose, so every bone frame equals
    /// its bind orientation until a joint is deliberately curled. Inverts the
    /// [x, -y, -z] world conversion the pipeline applies.
    fn bind_pose_landmarks_for(is_right: bool) -> [Landmark; 21] {
        let bp = if is_right { &BIND_POS_RIGHT } else { &BIND_POS_LEFT };
        let un = |p: [f32; 3]| fake_landmark(p[0], -p[1], -p[2]);
        let mut lm = [fake_landmark(0.0, 0.0, 0.0); 21];
        lm[WRIST] = un(bp[BONE_WRIST]);
        for (l, b) in [
            (THUMB_CMC, BONE_THUMB0),
            (THUMB_MCP, BONE_THUMB1),
            (THUMB_IP, BONE_THUMB2),
            (THUMB_TIP, BONE_THUMB3),
            (INDEX_MCP, BONE_INDEX1),
            (INDEX_PIP, BONE_INDEX2),
            (INDEX_DIP, BONE_INDEX3),
            (INDEX_TIP, BONE_INDEX4),
            (MIDDLE_MCP, BONE_MIDDLE1),
            (MIDDLE_PIP, BONE_MIDDLE2),
            (MIDDLE_DIP, BONE_MIDDLE3),
            (MIDDLE_TIP, BONE_MIDDLE4),
            (RING_MCP, BONE_RING1),
            (RING_PIP, BONE_RING2),
            (RING_DIP, BONE_RING3),
            (RING_TIP, BONE_RING4),
            (PINKY_MCP, BONE_PINKY1),
            (PINKY_PIP, BONE_PINKY2),
            (PINKY_DIP, BONE_PINKY3),
            (PINKY_TIP, BONE_PINKY4),
        ] {
            lm[l] = un(bp[b]);
        }
        lm
    }

    fn bind_pose_landmarks() -> [Landmark; 21] {
        bind_pose_landmarks_for(false)
    }

    /// Rodrigues rotation of `p` about the unit axis `palm` through `c`.
    fn rotate_about_palm(p: [f32; 3], c: [f32; 3], palm: [f32; 3], ang: f32) -> [f32; 3] {
        let v = sub3(p, c);
        let cr = vcross(palm, v);
        let dt = vdot(palm, v);
        let (s, co) = (ang.sin(), ang.cos());
        [
            c[0] + v[0] * co + cr[0] * s + palm[0] * dt * (1.0 - co),
            c[1] + v[1] * co + cr[1] * s + palm[1] * dt * (1.0 - co),
            c[2] + v[2] * co + cr[2] * s + palm[2] * dt * (1.0 - co),
        ]
    }

    /// Axis-angle of a unit quaternion, returning (axis, angle).
    fn quat_axis_angle(q: [f32; 4]) -> ([f32; 3], f32) {
        let w = q[0].clamp(-1.0, 1.0);
        let s = (q[1] * q[1] + q[2] * q[2] + q[3] * q[3]).sqrt();
        if s < 1e-9 {
            ([1.0, 0.0, 0.0], 0.0)
        } else {
            ([q[1] / s, q[2] / s, q[3] / s], 2.0 * s.atan2(w))
        }
    }

    /// A curled finger must not spin the skin around the bone. Each phalange
    /// frame is the rotation taking its bind segment onto its live segment;
    /// any residual twist about the live bone axis rotates the skinned
    /// geometry, which is what folds the rendered fingers forward instead of
    /// into the palm. Roll comes from the parent frame, so this has to hold at
    /// every curl depth, and the rendered flexion has to grow monotonically.
    #[test]
    fn finger_curl_adds_no_twist_about_the_bone_axis() {
        let bp = &BIND_POS_LEFT;
        let img = [fake_landmark(0.5, 0.55, 0.0); 21];
        let base = bind_pose_landmarks();
        let w = [base[WRIST].x, base[WRIST].y, base[WRIST].z];
        let palm = norm3(vcross(
            sub3([base[INDEX_MCP].x, base[INDEX_MCP].y, base[INDEX_MCP].z], w),
            sub3([base[MIDDLE_MCP].x, base[MIDDLE_MCP].y, base[MIDDLE_MCP].z], w),
        ));
        let (a, b) = ORIENT_SEG[BONE_INDEX3].unwrap();

        let mut prev_flexion = 0.0f32;
        for deg in [15.0f32, 30.0, 50.0, 70.0, 90.0] {
            let mut lm = base;
            let c = [lm[INDEX_DIP].x, lm[INDEX_DIP].y, lm[INDEX_DIP].z];
            let p = [lm[INDEX_TIP].x, lm[INDEX_TIP].y, lm[INDEX_TIP].z];
            let r = rotate_about_palm(p, c, palm, deg.to_radians());
            lm[INDEX_TIP] = fake_landmark(r[0], r[1], r[2]);

            let (wp, wr) = compose_chain(&compute_world_bones(&img, &lm, false));
            let bd = norm3(sub3(bp[b], bp[a]));
            let ld = norm3(sub3(wp[b], wp[a]));
            let applied = quat_mul(wr[BONE_INDEX3], quat_conj(BIND_QUAT_LEFT[BONE_INDEX3]));
            let (_, twist) = quat_axis_angle(quat_mul(applied, quat_conj(quat_from_to(bd, ld))));
            let twist = twist.to_degrees();
            assert!(
                twist < 5.0,
                "curl {deg} deg: {twist:.1} deg twist about the bone axis"
            );

            let flexion = angle_deg(bd, ld);
            assert!(
                flexion > prev_flexion,
                "curl {deg} deg: flexion {flexion:.1} deg did not grow past \
                 {prev_flexion:.1} deg"
            );
            prev_flexion = flexion;
        }
    }

    /// A hand already at the Valve bind pose must produce an identity wrist
    /// delta, both hands. Any standing offset between the bind basis and the
    /// live basis shows up here as a rotation that never goes away no matter
    /// how the wrist is turned.
    #[test]
    fn palm_basis_is_self_consistent_at_bind_pose() {
        let img = [fake_landmark(0.5, 0.55, 0.0); 21];
        for is_right in [false, true] {
            // Each hand needs its own table: the right bind pose is not the left
            // one mirrored, so feeding left landmarks to the right table would
            // report a disagreement that is the fixture's, not the math's.
            let lm = bind_pose_landmarks_for(is_right);
            let bones = compute_world_bones(&img, &lm, is_right);
            let (wpos, wrot) = compose_chain(&bones);
            // The wrist bone sits at the wrist in both frames, so the wrist's
            // world orientation IS the delta that was applied.
            let applied = quat_mul(wrot[BONE_WRIST], quat_conj(if is_right {
                BIND_QUAT_RIGHT[BONE_WRIST]
            } else {
                BIND_QUAT_LEFT[BONE_WRIST]
            }));
            let (axis, deg) = quat_axis_angle(applied);
            assert!(
                deg < 0.5,
                "right={is_right} bind pose gave {deg:.2} deg of wrist delta \
                 about {axis:?}; bind and live palm bases disagree"
            );
            let w = wpos[BONE_WRIST];
            assert!(
                (w[0] * w[0] + w[1] * w[1] + w[2] * w[2]).sqrt() < 1e-3,
                "right={is_right} wrist should sit at the model origin"
            );
        }
    }

    /// Angle between the wrist's applied rotation and identity, in degrees.
    /// Compares how far two palm-normal constructions displace the wrist.
    fn wrist_delta_deg(lm: &[Landmark; 21], palm_override: Option<[f32; 3]>, is_right: bool) -> f32 {
        let img = [fake_landmark(0.5, 0.55, 0.0); 21];
        let bones = compute_world_bones_with_palm(&img, lm, is_right, palm_override, None);
        let (_, wrot) = compose_chain(&bones);
        let bq = if is_right {
            &BIND_QUAT_RIGHT
        } else {
            &BIND_QUAT_LEFT
        };
        let applied = quat_mul(wrot[BONE_WRIST], quat_conj(bq[BONE_WRIST]));
        quat_axis_angle(applied).1.to_degrees()
    }

    /// The palm normal is the only input to the 3-DOF wrist roll, and it is
    /// built from landmark positions, so its noise lands on wrist roll in full.
    /// Displacing one MCP in depth must move the wrist less when the normal
    /// uses the whole knuckle row (Newell over 5 points) than when it comes
    /// from a single wrist x index cross product, whose two vectors are only
    /// ~17.6 deg apart and therefore amplify depth error by ~3.3x.
    #[test]
    fn palm_normal_uses_all_mcp_joints() {
        let mut lm = bind_pose_landmarks();
        // Nudge the pinky MCP 4 mm in depth — a realistic MediaPipe world
        // landmark error on the far edge of the knuckle row.
        lm[PINKY_MCP].z -= 0.004;

        let mut live = [[0.0f32; 3]; NUM_BONES];
        live[BONE_WRIST] = [lm[WRIST].x, lm[WRIST].y, lm[WRIST].z];
        for (bone, l) in [
            (BONE_INDEX1, INDEX_MCP),
            (BONE_MIDDLE1, MIDDLE_MCP),
            (BONE_RING1, RING_MCP),
            (BONE_PINKY1, PINKY_MCP),
        ] {
            live[bone] = [lm[l].x, lm[l].y, lm[l].z];
        }
        let plane = palm_plane_normal(&live).expect("palm ring is non-degenerate");
        let cross2 = {
            let a = sub3(live[BONE_INDEX1], live[BONE_WRIST]);
            let b = sub3(live[BONE_MIDDLE1], live[BONE_WRIST]);
            norm3(vcross(a, b))
        };

        let via_plane = wrist_delta_deg(&lm, Some(plane), false);
        let via_cross = wrist_delta_deg(&lm, Some(cross2), false);
        assert!(
            via_plane < via_cross,
            "5-point plane normal moved the wrist {via_plane:.2} deg, \
             2-point cross product moved it {via_cross:.2} deg — the plane \
             construction is supposed to be the quieter one"
        );
    }

/// Valve's two bind tables are x-mirrors of each other (det < 0), so Newell's
    /// method returns the same +X-ward normal on both and the pipeline's
    /// `palm_sign` supplies the handedness. Pin the post-sign result against
    /// Valve's bind orientation (left palm faces +X, right faces -X): getting
    /// the winding backwards mirrors the palm, which renders as a glove worn on
    /// the back of the hand.
    #[test]
    fn palm_normal_sign_matches_valve_bind_orientation() {
        // Newell is sign-agnostic about which face it calls out, so winding it
        // backwards mirrors the palm and renders as a glove on the back of
        // the hand. The thumb orients it, and it must land on Valve's bind
        // orientation: left palm +X, right palm -X.
        let left = palm_plane_normal(&BIND_POS_LEFT).expect("left palm ring");
        assert!(
            left[0] > 0.9,
            "left bind palm normal x={:.3} should point +X per Valve's bind pose",
            left[0]
        );
        // The right table is the left reflected through x (M = diag(-1,1,1), det -1).
        // A normal is a pseudovector, so it maps as det(M) * M * n, which flips
        // y and z and leaves x alone. Production's palm_sign = -1 then lands
        // the right hand on -X, matching Valve.
        let right = palm_plane_normal(&BIND_POS_RIGHT).expect("right palm ring");
        assert!(
            right[0] < -0.9,
            "right bind palm normal x={:.3} should point -X per Valve's bind pose",
            right[0]
        );
        // The thumb decides the sign, not the raw winding.
        for (bp, want) in [(&BIND_POS_LEFT, 1.0f32), (&BIND_POS_RIGHT, -1.0f32)] {
            let n = palm_plane_normal(bp).expect("ring");
            let thumb = norm3(sub3(bp[BONE_THUMB0], bp[BONE_WRIST]));
            let d = n[0] * thumb[0] + n[1] * thumb[1] + n[2] * thumb[2];
            assert!(
                d < 0.0,
                "palm normal {n:?} should face away from the thumb, dot={d}"
            );
            assert!(
                (n[0] * want).abs() > 0.9,
                "palm normal x={:.3} should land on {want:+.0}X",
                n[0]
            );
        }
    }

    /// Real captured hand, on the real geometry the previous assertions were
    /// measured from. MCP/PIP/DIP/TIP are copied from the landmarks, so they
    /// must land exactly on the bind joints, and each metacarpal stub must sit
    /// on Valve's axis at Valve's length — which is what keeps the error out of
    /// every finger chain below it.
    #[test]
    fn captured_hand_reconstructs_bind_joints_exactly() {
        let lm = captured_world();
        let pos = compute_positions(&image_hand(), &lm, false);
        for (lm_id, bone) in [
            (INDEX_MCP, BONE_INDEX1),
            (MIDDLE_DIP, BONE_MIDDLE3),
            (PINKY_TIP, BONE_PINKY4),
        ] {
            let got = [lm[lm_id].x, -lm[lm_id].y, -lm[lm_id].z];
            let d = sub3(pos[bone], got);
            let e = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
            assert!(e < 1e-6, "bone {bone} sits {e} m from landmark {lm_id}");
        }
        for (stub, mcp) in [
            (BONE_INDEX0, BONE_INDEX1),
            (BONE_MIDDLE0, BONE_MIDDLE1),
            (BONE_RING0, BONE_RING1),
            (BONE_PINKY0, BONE_PINKY1),
        ] {
            let d = sub3(pos[mcp], pos[stub]);
            let l = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
            // The bind stub→MCP distance, not the wrist→stub one. Valve's two
            // are 74 vs 26 mm on the index, so a length taken from the wrong
            // segment passes every orientation check and still collapses the
            // knuckle row.
            let len = vnorm(sub3(BIND_POS_LEFT[mcp], BIND_POS_LEFT[stub]));
            assert!(
                (l - len).abs() < 1e-6,
                "bone {stub}->{mcp} is {l} m, bind length {len} m"
            );
            let axis = norm3(sub3(BIND_POS_LEFT[mcp], BIND_POS_LEFT[stub]));
            let dir = norm3(d);
            let dot = dir[0] * axis[0] + dir[1] * axis[1] + dir[2] * axis[2];
            assert!(
                dot > 0.9999,
                "bone {stub} stub axis is {:.2} deg off Valve's metacarpal axis",
                angle_deg(dir, axis)
            );
        }
    }

    /// Rolling the wrist must carry the whole hand, finger chains included,
    /// by exactly that rotation. The metacarpal stub lives in model space, so
    /// this is what catches it being pinned to the bind tables instead of
    /// following the hand — which shears the fingers on a wrist turn.
    #[test]
    fn wrist_roll_carries_the_whole_skeleton() {
        let base = captured_world();
        let img = image_hand();
        let (_, rot_base) = compose_chain(&world_bones(&img, &base));
        let axis = [0.0f32, 1.0, 0.0];
        let half = (20.0f32).to_radians() * 0.5;
        let q_roll = [half.cos(), axis[0] * half.sin(), axis[1] * half.sin(), axis[2] * half.sin()];
        let rolled: [Landmark; 21] = std::array::from_fn(|i| {
            let p = [base[i].x, -base[i].y, -base[i].z];
            let pivot = [base[0].x, -base[0].y, -base[0].z];
            let d = sub3(p, pivot);
            let r = quat_rot_vec(q_roll, d);
            fake_landmark(
                pivot[0] + r[0],
                -(pivot[1] + r[1]),
                -(pivot[2] + r[2]),
            )
        });
        let (_, rot_rolled) = compose_chain(&world_bones(&img, &rolled));
        for &i in &[
            BONE_WRIST,
            BONE_INDEX0,
            BONE_INDEX1,
            BONE_INDEX2,
            BONE_MIDDLE0,
            BONE_MIDDLE1,
            BONE_MIDDLE2,
            BONE_PINKY0,
            BONE_PINKY1,
        ] {
            let skin_base = quat_mul(rot_base[i], quat_conj(BIND_QUAT_LEFT[i]));
            let skin_rolled = quat_mul(rot_rolled[i], quat_conj(BIND_QUAT_LEFT[i]));
            let want = quat_mul(q_roll, skin_base);
            let dot = (want[0] * skin_rolled[0]
                + want[1] * skin_rolled[1]
                + want[2] * skin_rolled[2]
                + want[3] * skin_rolled[3])
                .abs();
            assert!(
                dot > 0.99,
                "bone {i} skin does not follow the wrist roll, dot={dot:.4}"
            );
        }
    }

    /// Rolling the wrist one degree at a time must produce a quaternion stream
    /// that never flips sign. `q` and `-q` are the same rotation, so the
    /// geometry is unaffected, but SteamVR slerps between the frames it is
    /// handed and the short way from `q` to `-q` is a full 360° — so a single
    /// sign flip reads as the hand spinning once mid-roll.
    ///
    /// The old matrix-to-quaternion conversion picked its sign from whichever
    /// of its four branches ran, and adjacent branches disagree, so the sign
    /// flipped at fixed points in the roll (measured at 172° and 352° of a full
    /// turn, dot −0.99985 between neighbours). Canonicalizing the sign removes
    /// it: the largest component is always positive, with ties going to the
    /// earliest index so the rule stays total.
    #[test]
    fn rolling_the_wrist_never_flips_a_quaternion_sign() {
        let base = captured_world();
        let img = image_hand();
        let pivot = [base[0].x, -base[0].y, -base[0].z];
        let mut prev: Option<[[f32; 4]; NUM_BONES]> = Some(
            std::array::from_fn(|_| [1.0f32, 0.0, 0.0, 0.0]),
        );
        for step in 0..360 {
            let a = (step as f32).to_radians();
            let q_roll = [a.cos(), a.sin(), 0.0, 0.0];
            let rolled: [Landmark; 21] = std::array::from_fn(|i| {
                let p = [base[i].x, -base[i].y, -base[i].z];
                let r = quat_rot_vec(q_roll, sub3(p, pivot));
                fake_landmark(
                    pivot[0] + r[0],
                    -(pivot[1] + r[1]),
                    -(pivot[2] + r[2]),
                )
            });
            let bones = compute_world_bones_tracked(
                &img,
                &rolled,
                false,
                None,
                prev.as_ref().unwrap(),
            );
            if let Some(p) = prev {
                for i in 0..NUM_BONES {
                    let dot: f32 = bones[i]
                        .orientation
                        .iter()
                        .zip(p[i].iter())
                        .map(|(x, y)| x * y)
                        .sum();
                    assert!(
                        dot >= 0.0,
                        "bone {i} flipped quaternion sign at roll step {step} \
                         (dot {dot:+.5} between neighbours)"
                    );
                }
            }
            prev = Some(bones.map(|b| b.orientation));
        }
    }

    #[test]
    fn splay_survives_per_segment() {
        let img = image_hand();
        let base = captured_world();
        let mut live = [[0.0f32; 3]; NUM_BONES];
        for (bone, lm) in [
            (BONE_WRIST, WRIST),
            (BONE_THUMB0, THUMB_CMC),
            (BONE_INDEX1, INDEX_MCP),
            (BONE_MIDDLE1, MIDDLE_MCP),
            (BONE_RING1, RING_MCP),
            (BONE_PINKY1, PINKY_MCP),
        ] {
            let l = base[lm];
            live[bone] = [l.x, -l.y, -l.z];
        }
        let palm = palm_plane_normal(&live).expect("palm ring");
        for (name, chain, bones) in [
            (
                "index",
                [INDEX_MCP, INDEX_PIP, INDEX_DIP, INDEX_TIP],
                [BONE_INDEX1, BONE_INDEX2, BONE_INDEX3, BONE_INDEX4],
            ),
            (
                "middle",
                [MIDDLE_MCP, MIDDLE_PIP, MIDDLE_DIP, MIDDLE_TIP],
                [BONE_MIDDLE1, BONE_MIDDLE2, BONE_MIDDLE3, BONE_MIDDLE4],
            ),
            (
                "ring",
                [RING_MCP, RING_PIP, RING_DIP, RING_TIP],
                [BONE_RING1, BONE_RING2, BONE_RING3, BONE_RING4],
            ),
            (
                "pinky",
                [PINKY_MCP, PINKY_PIP, PINKY_DIP, PINKY_TIP],
                [BONE_PINKY1, BONE_PINKY2, BONE_PINKY3, BONE_PINKY4],
            ),
        ] {
            let (neutral, _) = compose_chain(&world_bones(&img, &base));
            for seg in 0..3 {
                // Abduct this phalanx and everything distal to it, about the
                // palm normal through its own proximal joint.
                let mut lm = base;
                for k in seg..4 {
                    let id = chain[k];
                    let p = [lm[id].x, -lm[id].y, -lm[id].z];
                    let pivot = [lm[chain[seg]].x, -lm[chain[seg]].y, -lm[chain[seg]].z];
                    let r = rotate_about_palm(p, pivot, palm, 30.0f32.to_radians());
                    lm[id] = fake_landmark(r[0], -r[1], -r[2]);
                }
                let (pos, _) = compose_chain(&world_bones(&img, &lm));
                let want = norm3(vsub(
                    [lm[chain[seg + 1]].x, -lm[chain[seg + 1]].y, -lm[chain[seg + 1]].z],
                    [lm[chain[seg]].x, -lm[chain[seg]].y, -lm[chain[seg]].z],
                ));
                let neutral_dir = norm3(vsub(neutral[bones[seg + 1]], neutral[bones[seg]]));
                let now = norm3(vsub(pos[bones[seg + 1]], pos[bones[seg]]));
                let got = angle_deg(neutral_dir, now);
                let expected = angle_deg(neutral_dir, want);
                assert!(
                    (got - expected).abs() < 0.5,
                    "{name} segment {seg}: bones turned {got:.1} deg where the \
                     landmarks say {expected:.1} deg — splay is being lost"
                );
            }
        }
    }
}
