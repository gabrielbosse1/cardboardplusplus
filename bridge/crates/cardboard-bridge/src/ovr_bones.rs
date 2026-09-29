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

/// Valve GLB metacarpal lengths (wrist→MCP distance in mm→m), same as hand_tracker.py.
/// Index 25.8mm, Middle 17.9mm, Ring 17.6mm, Pinky 24.5mm.
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

/// Short metacarpal: place Finger0 along wrist→MCP at Valve's GLB distance.
fn short_meta(wrist: Landmark, mcp: Landmark, target_len: f32) -> Landmark {
    let d = sub(mcp, wrist);
    let l = norm(d);
    if l < 1e-9 {
        return mcp;
    }
    add(wrist, scale(d, target_len / l))
}

/// Compute OpenVR bones from 21 MediaPipe landmarks.
///
/// The algorithm does NOT modify the original landmarks. It adds 10 new bones:
/// - BONE_ROOT(0), BONE_WRIST(1): both at wrist position
/// - 4 metacarpals (INDEX0=6, MIDDLE0=11, RING0=16, PINKY0=21): wrist→MCP at Valve length
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

fn vadd(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
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
fn mat_to_quat(m: [[f32; 3]; 3]) -> [f32; 4] {
    let (m00, m01, m02) = (m[0][0], m[0][1], m[0][2]);
    let (m10, m11, m12) = (m[1][0], m[1][1], m[1][2]);
    let (m20, m21, m22) = (m[2][0], m[2][1], m[2][2]);
    let trace = m00 + m11 + m22;
    if trace > 0.0 {
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
    }
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

/// Metacarpal lengths from Valve's GLB skeleton (meters), keyed by bone index.
const META_LEN_WORLD: [(usize, f32); 4] = [
    (BONE_INDEX0, 0.0258),
    (BONE_MIDDLE0, 0.0179),
    (BONE_RING0, 0.0176),
    (BONE_PINKY0, 0.0245),
];

/// Bind-pose segment used to orient each bone: model-space direction from
/// bone A to bone B, matched to the live segment (same indices). `None` =
/// reuse the previous segment's delta (tips, aux) or identity (root).
/// Mirrors hand_tracker.py JDIR; directions come from Valve's skeleton GLBs.
const ORIENT_SEG: [Option<(usize, usize)>; NUM_BONES] = [
    None,                             // 0 root
    Some((BONE_WRIST, BONE_MIDDLE0)), // 1 wrist
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
    compute_world_bones_with_palm(image_landmarks, world_landmarks, is_right_hand, None)
}

/// Same skeleton with a caller-supplied palm normal in MediaPipe axes (same
/// convention as `world_landmarks`; converted to OpenVR axes inside). A
/// missing or degenerate override falls back to the position-derived normal.
pub fn compute_world_bones_with_palm(
    image_landmarks: &[Landmark; 21],
    world_landmarks: &[Landmark; 21],
    is_right_hand: bool,
    palm: Option<[f32; 3]>,
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

    // Fingers: metacarpal stubs at Valve GLB length, MCP/PIP/DIP/TIP direct
    let meta = [
        (BONE_INDEX0, lm[INDEX_MCP], META_LEN_WORLD[0].1),
        (BONE_MIDDLE0, lm[MIDDLE_MCP], META_LEN_WORLD[1].1),
        (BONE_RING0, lm[RING_MCP], META_LEN_WORLD[2].1),
        (BONE_PINKY0, lm[PINKY_MCP], META_LEN_WORLD[3].1),
    ];
    for (bone, mcp, len) in &meta {
        let d = vsub(*mcp, w);
        let l = vnorm(d);
        positions[*bone] = if l < 1e-9 {
            *mcp
        } else {
            vadd(w, vscale(d, len / l))
        };
    }

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

    // Palm normal from the live/bind hand basis (same formula both sides,
    // chirality sign so left bind → +X, right bind → −X). Used only for the
    // wrist roll: shortest-arc along wrist→middle0 leaves twist free, which
    // is what put the palm on the back of the hand.
    let palm_sign = if is_right_hand { -1.0 } else { 1.0 };
    let palm_of = |p: &[[f32; 3]; NUM_BONES]| -> [f32; 3] {
        let idx = vsub(p[BONE_INDEX0], p[BONE_WRIST]);
        let mid = vsub(p[BONE_MIDDLE0], p[BONE_WRIST]);
        vscale(vnormalize(vcross(idx, mid)), palm_sign)
    };
    let palm_bind = palm_of(bind_pos);
    let palm_live = match palm {
        Some(p) if vnorm(p) > 1e-9 => vscale(vnormalize([p[0], -p[1], -p[2]]), palm_sign),
        _ => palm_of(&positions),
    };
    let finger_bind = vsub(bind_pos[BONE_MIDDLE0], bind_pos[BONE_WRIST]);
    let finger_live = vsub(positions[BONE_MIDDLE0], positions[BONE_WRIST]);

    let mut frames = [[1.0f32, 0.0, 0.0, 0.0]; NUM_BONES];
    let mut prev_delta = [1.0f32, 0.0, 0.0, 0.0];
    for i in 0..NUM_BONES {
        if i == BONE_WRIST {
            // Full 3-DOF wrist: map bind (finger, palm) → live (finger, palm).
            // Curl joints are untouched; this only rolls the glove so the
            // palm faces the same way as the landmarks. Degenerate live
            // basis (zero landmarks): finger-only delta, else identity.
            let delta = if vnorm(finger_live) > 1e-9 && vnorm(palm_live) > 1e-9 {
                quat_from_basis(finger_bind, palm_bind, finger_live, palm_live)
            } else if vnorm(finger_live) > 1e-9 && vnorm(finger_bind) > 1e-9 {
                quat_from_to(vnormalize(finger_bind), vnormalize(finger_live))
            } else {
                [1.0, 0.0, 0.0, 0.0]
            };
            prev_delta = delta;
            frames[i] = quat_mul(delta, bind_q[i]);
            continue;
        }
        match ORIENT_SEG[i] {
            Some((a, b)) => {
                let bd = vnormalize(vsub(bind_pos[b], bind_pos[a]));
                let ld = vnormalize(vsub(positions[b], positions[a]));
                let delta = if vnorm(bd) > 1e-9 && vnorm(ld) > 1e-9 {
                    // Two-vector frame: the segment aligns the bone axis and
                    // the palm normal carries the roll (projected palm normal
                    // equals the nail direction for a finger curling in the
                    // palm plane). A single arc never adds twist about the
                    // segment axis, so a spin about the finger axis would
                    // leave the skin unrotated.
                    // ponytail: palm reference dies past ~72° flexion
                    // (|proj| = cosθ < 0.3) — roll freezes at a deep fist.
                    let pb = vsub(palm_bind, vscale(bd, vdot(palm_bind, bd)));
                    let pl = vsub(palm_live, vscale(ld, vdot(palm_live, ld)));
                    if vnorm(pb) > 0.3 && vnorm(pl) > 0.3 {
                        quat_from_basis(bd, palm_bind, ld, palm_live)
                    } else {
                        quat_from_to(bd, ld)
                    }
                } else {
                    prev_delta
                };
                prev_delta = delta;
                frames[i] = quat_mul(delta, bind_q[i]);
            }
            None => {
                frames[i] = if i == BONE_ROOT {
                    [1.0, 0.0, 0.0, 0.0]
                } else {
                    quat_mul(prev_delta, bind_q[i])
                };
            }
        }
    }

    // Aux bones (26-30) sit after the pinky chain in the loop, so the
    // None-branch prev_delta above would give every one of them the pinky's
    // last-segment delta. Each aux tracks its own finger's tip instead;
    // bind quats for aux/tip pairs are identical in the GLB, so sharing the
    // tip's model-space frame keeps orientation and position in sync.
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
        let bones = world_bones(&image_hand(), &metric_hand());
        // Synthetic wrist→MCP is ~0.05-0.11m (2-4x Valve); metacarpal bones
        // must be snapped to Valve's GLB lengths regardless.
        let expected = [0.0258, 0.0179, 0.0176, 0.0245];
        let slots = [BONE_INDEX0, BONE_MIDDLE0, BONE_RING0, BONE_PINKY0];
        for (slot, exp) in slots.iter().zip(expected.iter()) {
            let p = bones[*slot].position;
            let d = (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]).sqrt();
            assert!(
                (d - exp).abs() < 0.002,
                "bone {slot} len {d}, expected {exp}"
            );
        }
    }

    /// Isolates quat_from_basis: rotating only the live pair by R must
    /// left-multiply the resulting delta by exactly R (bind side untouched).
    #[test]
    fn basis_delta_follows_live_rotation() {
        // Exact vectors printed from the wrist branch (metric_hand +60° spin).
        let fb = [0.002177f32, 0.0071199997, 0.016319];
        let pb = [0.9469149f32, 0.2285864, -0.22605363];
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
        let base = metric_hand();
        let (s, c) = (60.0f32.to_radians().sin(), 60.0f32.to_radians().cos());
        let spun = base.map(|l| fake_landmark(c * l.x + s * l.z, l.y, -s * l.x + c * l.z));
        let img = image_hand();
        let (_, rot_base) = compose_chain(&world_bones(&img, &base));
        let (_, rot_spun) = compose_chain(&world_bones(&img, &spun));
        // Landmarks spin +60° about MediaPipe +Y; the [x,-y,-z] axis flip
        // conjugates that to -60° about OpenVR +Y.
        let half = 30.0f32.to_radians();
        let q_spin = [half.cos(), 0.0, -half.sin(), 0.0];
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

    /// Right-hand bone +X must point proximal (toward the parent),
    /// matching the right vr_glove bind pose (children along -X).
    /// Positions are never mirrored: the chain still lands on the true joints.
    #[test]
    fn world_right_x_axis_points_proximal() {
        let bones = compute_world_bones(&image_hand(), &metric_hand(), true);
        let (wpos, wrot) = compose_chain(&bones);
        for (bone, parent) in [(BONE_INDEX1, BONE_INDEX0), (BONE_MIDDLE1, BONE_MIDDLE0)] {
            let x_axis = quat_rot_vec(wrot[bone], [1.0, 0.0, 0.0]);
            let to_parent = norm3(sub3(wpos[parent], wpos[bone]));
            let dot =
                x_axis[0] * to_parent[0] + x_axis[1] * to_parent[1] + x_axis[2] * to_parent[2];
            assert!(
                dot > 0.9,
                "right bone {bone} +X should point at parent, dot={dot}"
            );
        }
        // Chain still reconstructs the true model joints (no mirror).
        let tip = wpos[BONE_MIDDLE4];
        let dt = (tip[0] * tip[0] + tip[1] * tip[1] + tip[2] * tip[2]).sqrt();
        assert!((dt - 0.24).abs() < 0.05, "right middle tip span {dt}");
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
        wlm[INDEX_TIP] = fake_landmark(0.0, 0.1, 0.05);
        let bones = world_bones(&image_hand(), &wlm);
        let (wpos, _) = compose_chain(&bones);
        let z = wpos[BONE_INDEX4][2];
        assert!((z + 0.05).abs() < 1e-5, "tip depth {z}, expected ~-0.05");
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
        let mut wlm = metric_hand();
        wlm[INDEX_TIP].z -= 0.03;
        let (pos, _) = compose_chain(&compute_world_bones(&img, &wlm, false));
        let (base, _) = compose_chain(&compute_world_bones(&img, &metric_hand(), false));
        let swing = pos[BONE_INDEX4][2] - base[BONE_INDEX4][2];
        assert!(
            swing > 0.02,
            "tip should track world z toward the user, swing {swing}"
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
            let palm_live = {
                let idx = sub3(wpos[BONE_INDEX0], wpos[BONE_WRIST]);
                let mid = sub3(wpos[BONE_MIDDLE0], wpos[BONE_WRIST]);
                let c = [
                    idx[1] * mid[2] - idx[2] * mid[1],
                    idx[2] * mid[0] - idx[0] * mid[2],
                    idx[0] * mid[1] - idx[1] * mid[0],
                ];
                let n = norm3(c);
                [n[0] * sign, n[1] * sign, n[2] * sign]
            };
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
            let idx_b = sub3(bp[BONE_INDEX0], bp[BONE_WRIST]);
            let mid_b = sub3(bp[BONE_MIDDLE0], bp[BONE_WRIST]);
            let cb = [
                idx_b[1] * mid_b[2] - idx_b[2] * mid_b[1],
                idx_b[2] * mid_b[0] - idx_b[0] * mid_b[2],
                idx_b[0] * mid_b[1] - idx_b[1] * mid_b[0],
            ];
            let nb = norm3(cb);
            let palm_bind = [nb[0] * sign, nb[1] * sign, nb[2] * sign];
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

    /// The composed wrist orientation must take the bind wrist→middle0
    /// segment onto the live one (bind+delta tracks palm roll).
    #[test]
    fn wrist_follows_palm_rotation() {
        let bones = world_bones(&image_hand(), &metric_hand());
        assert_bind_delta_maps_segments(&bones, false, "metric");
        let (wpos, wrot) = compose_chain(&bones);
        let bd = norm3(sub3(BIND_POS_LEFT[BONE_MIDDLE0], BIND_POS_LEFT[BONE_WRIST]));
        let ld = norm3(sub3(wpos[BONE_MIDDLE0], wpos[BONE_WRIST]));
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
        let b = compute_world_bones_with_palm(&img, &wlm, false, Some([0.0, 0.0, 0.0]));
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
        let b = compute_world_bones_with_palm(&img, &wlm, false, Some([1.0, 0.0, 0.0]));
        let mut diff = 0.0f32;
        for k in 0..4 {
            diff = diff.max((a[BONE_WRIST].orientation[k] - b[BONE_WRIST].orientation[k]).abs());
        }
        assert!(
            diff > 1e-3,
            "override should move the wrist frame, diff={diff}"
        );
    }

    #[test]
    fn palm_override_preserves_left_and_right_wrist_frames() {
        let img = image_hand();
        let wlm = metric_hand();
        let idx = [
            wlm[INDEX_MCP].x - wlm[WRIST].x,
            wlm[INDEX_MCP].y - wlm[WRIST].y,
            wlm[INDEX_MCP].z - wlm[WRIST].z,
        ];
        let mid = [
            wlm[MIDDLE_MCP].x - wlm[WRIST].x,
            wlm[MIDDLE_MCP].y - wlm[WRIST].y,
            wlm[MIDDLE_MCP].z - wlm[WRIST].z,
        ];
        let palm = vnormalize(vcross(idx, mid));
        for is_right in [false, true] {
            let fallback = compute_world_bones(&img, &wlm, is_right);
            let overridden = compute_world_bones_with_palm(&img, &wlm, is_right, Some(palm));
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

    /// normalize_segments rescales every PARENT edge to its Valve bind length
    /// along the live direction, so it must hand the landmark joint angles
    /// back untouched. A finger that curls while the hand is flat therefore
    /// comes from the landmarks, not from this function — on the captured
    /// hand the composed chain matches the landmarks at every joint.
    #[test]
    fn normalize_preserves_every_landmark_joint_angle() {
        let lm = captured_world();
        let img = [fake_landmark(0.5, 0.55, 0.0); 21];
        let (wp, _) = compose_chain(&compute_world_bones_with_palm(&img, &lm, false, None));
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
}
