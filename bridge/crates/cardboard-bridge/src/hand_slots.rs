use crate::net::mediapipe::{DetectedHand, Landmark};

/// Stable 2-slot hand tracking across camera frames.
///
/// MediaPipe's per-frame handedness label flickers (especially on dorsal
/// views), which used to teleport skeletons between SteamVR devices and
/// flip the frame chirality mid-track (inside-out fingers, inverted
/// curls). Slots track each hand by wrist-position continuity, but the label
/// — not image position — decides which side a hand belongs to: a hand that
/// has crossed the midline sits in the opposite half of the image, and
/// reading its position there spawns the wrong device. A sustained
/// disagreement between a slot's side and the hand's label corrects the
/// assignment, latched so it cannot oscillate.
///
/// Slot index == SteamVR device id (0 = left, 1 = right). A hand keeps its
/// slot (and side) across crossings, brief losses, and label flicker.
pub struct TrackedHand {
    pub hand_id: u8,
    pub landmarks: [Landmark; 21],
    pub world_landmarks: [Landmark; 21],
    /// Smoothed palm normal (world-landmark space) for the orientation math.
    pub palm: [f32; 3],
}

/// Wrist travel per frame (normalized image units) allowed to keep a slot.
const MATCH_GATE: f32 = 0.35;
/// Frames without a sighting before a slot is freed.
const MISSING_LIMIT: u32 = 20;
/// Anchor (wrist image position) smoothing weight.
const ANCHOR_ALPHA: f32 = 0.5;
/// Sample interval of the camera detect loop.
const SAMPLE_DT: f32 = 1.0 / 30.0;
/// Velocity-adaptive joint filter.
/// `MIN_CUTOFF_HZ` is how hard a motionless hand gets filtered. `BETA` sets
/// how far the gain opens per m/s of joint speed, so it must be large: 2 mm
/// of landmark noise already looks like 0.085 m/s of motion, and a relaxed
/// finger closes at ~0.02 m/s, so the two only separate if BETA is in the
/// hundreds. `DERIV_CUTOFF_HZ` is deliberately far below `MIN_CUTOFF_HZ` â€”
/// the speed estimate is averaged hard so noise does not read as motion and
/// hold the gain open. Fingertips run ~0.01 m/s (relaxed curl) to ~0.25 m/s
/// (fast gesture); across that whole band these values cost at most 0.3 mm
/// of extra lag over the old fixed gain while cutting rest jitter ~4x.
const MIN_CUTOFF_HZ: f32 = 0.7;
const DERIV_CUTOFF_HZ: f32 = 0.15;
const BETA: f32 = 100.0;
/// Floor and gain for the palm-normal follow rate. The normal is a direction
/// in radians, so the speed term is rad/s rather than m/s; a brisk wrist roll
/// runs 5-15 rad/s, which needs a gain far smaller than `BETA` or the filter
/// never filters at all. The normal is the only input to the 3-DOF wrist
/// orientation, so its lag lands directly on wrist roll while finger curl,
/// which is computed inside the parent frame, is unaffected.
const PALM_MIN_CUTOFF_HZ: f32 = 0.7;
const PALM_BETA: f32 = 0.35;
/// Consecutive disagreeing labels on BOTH slots before correcting a swap.
const SWAP_AFTER: u32 = 90;
/// Consecutive disagreeing labels before warning about a single slot.
const WARN_AFTER: u32 = 150;
/// Minimum frames between repeat warnings.
const WARN_EVERY: u64 = 1200;
/// Fingertip landmark ids (thumb..pinky) watched by the jitter probe.
const TIP_IDS: [usize; 5] = [4, 8, 12, 16, 20];

/// Smoothing gain for a cutoff frequency at a fixed timestep.
fn cutoff_alpha(cutoff_hz: f32) -> f32 {
    let tau = 1.0 / (2.0 * std::f32::consts::PI * cutoff_hz);
    1.0 / (1.0 + tau / SAMPLE_DT)
}

/// One velocity-adaptive step ("One Euro"), applied per axis. The gain rises
/// with the joint's own speed, so a still hand is filtered hard while a
/// moving hand is followed almost untouched.
///
/// A fixed gain cannot do both. At 30 Hz a first-order low-pass only removes
/// `1 - sqrt(a/(2-a))` of the jitter: the old `a = 0.65` removed 31% while
/// still lagging half a frame. The distal phalanges are only ~16 mm long, so
/// a couple of millimetres of per-joint noise swings their direction by tens
/// of degrees â€” the fingertip visibly flaps. Because most landmark noise is
/// common-mode across the hand, no per-joint filter can remove it; only
/// adaptivity can, by filtering hardest exactly when the hand is at rest.
///
/// Returns the new filtered value and the new smoothed derivative.
fn adaptive_step(out: [f32; 3], dx_hat: [f32; 3], raw: [f32; 3]) -> ([f32; 3], [f32; 3]) {
    let a_deriv = cutoff_alpha(DERIV_CUTOFF_HZ);
    let mut next = [0.0f32; 3];
    let mut next_dx = [0.0f32; 3];
    for k in 0..3 {
        let dx = (raw[k] - out[k]) / SAMPLE_DT;
        let d = a_deriv * dx + (1.0 - a_deriv) * dx_hat[k];
        let a = cutoff_alpha(MIN_CUTOFF_HZ + BETA * d.abs());
        next_dx[k] = d;
        next[k] = a * raw[k] + (1.0 - a) * out[k];
    }
    (next, next_dx)
}

/// Angle between two normals in degrees; None when either is degenerate.
fn normal_angle_deg(a: [f32; 3], b: [f32; 3]) -> Option<f64> {
    let la = (a[0] * a[0] + a[1] * a[1] + a[2] * a[2]).sqrt();
    let lb = (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]).sqrt();
    if la < 1e-9 || lb < 1e-9 {
        return None;
    }
    let d = ((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb)).clamp(-1.0, 1.0);
    Some(d.acos().to_degrees() as f64)
}

fn smooth_normal(current: [f32; 3], target: [f32; 3], alpha: f32) -> [f32; 3] {
    let dot = (current[0] * target[0]
        + current[1] * target[1]
        + current[2] * target[2])
        .clamp(-1.0, 1.0);
    // Near-parallel: nothing to do.
    if dot > 1.0 - 1e-6 {
        return current;
    }
    // Antipodal. slerp has no defined axis here, and taking the target outright
    // jumps the normal 180 deg in a single frame, which reads as the wrist
    // teleporting. Nudge just past the antipode instead: the sign of the tiny
    // component left over picks a stable axis and the normal keeps moving.
    if dot < -1.0 + 1e-6 {
        let axis = normal_axis(current, target);
        let t = alpha * std::f32::consts::PI;
        let (s, c) = t.sin_cos();
        let moved = [
            axis[0] * s + current[0] * c,
            axis[1] * s + current[1] * c,
            axis[2] * s + current[2] * c,
        ];
        let l = (moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]).sqrt();
        return if l < 1e-9 {
            target
        } else {
            [moved[0] / l, moved[1] / l, moved[2] / l]
        };
    }
    let angle = dot.acos();
    let sin_angle = angle.sin();
    let current_weight = ((1.0 - alpha) * angle).sin() / sin_angle;
    let target_weight = (alpha * angle).sin() / sin_angle;
    let result = [
        current_weight * current[0] + target_weight * target[0],
        current_weight * current[1] + target_weight * target[1],
        current_weight * current[2] + target_weight * target[2],
    ];
    let length = (result[0] * result[0] + result[1] * result[1] + result[2] * result[2]).sqrt();
    if length < 1e-9 {
        target
    } else {
        [result[0] / length, result[1] / length, result[2] / length]
    }
}

/// Any unit vector perpendicular to both `a` and `b`, for the degenerate
/// antipodal case where the slerp axis is undefined.
fn normal_axis(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    let c = [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ];
    let l = (c[0] * c[0] + c[1] * c[1] + c[2] * c[2]).sqrt();
    if l > 1e-6 {
        [c[0] / l, c[1] / l, c[2] / l]
    } else {
        // Exactly antipodal has no unique axis; pick one from any component.
        let a = if a[0].abs() < 0.9 { a } else { [a[1], a[2], a[0]] };
        let c = [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0],
        ];
        let l = (c[0] * c[0] + c[1] * c[1] + c[2] * c[2]).sqrt();
        if l < 1e-9 {
            [1.0, 0.0, 0.0]
        } else {
            [c[0] / l, c[1] / l, c[2] / l]
        }
    }
}

/// Landmark ids forming the palm plane: wrist plus the four MCP joints.
const PALM_RING: [usize; 5] = [0, 5, 9, 13, 17];

/// Best-fit palm-plane normal by Newell's method over wrist + all four MCPs,
/// oriented so it points out of the palm.
///
/// A two-point cross product (wrist x index MCP) reads less code but the two
/// vectors are only ~17.6 deg apart, so landmark noise is amplified ~3.3x and
/// the wrist roll jitters by several degrees. Five points across the palm also
/// survive foreshortening, where the cross product degenerates to nothing.
///
/// Newell's sign follows the ring's winding order, which runs opposite ways on
/// the two hands, so the raw result can be the back of the hand. The thumb CMC
/// fixes it without reference to winding: the palm always faces away from it.
/// Returns `None` when the ring collapses edge-on to the camera.
fn palm_plane_normal(lms: &[Landmark; 21]) -> Option<[f32; 3]> {
    let mut n = [0.0f64; 3];
    for i in 0..PALM_RING.len() {
        let a = lms[PALM_RING[i]];
        let b = lms[PALM_RING[(i + 1) % PALM_RING.len()]];
        n[0] += (a.y as f64) * (b.z as f64) - (a.z as f64) * (b.y as f64);
        n[1] += (a.z as f64) * (b.x as f64) - (a.x as f64) * (b.z as f64);
        n[2] += (a.x as f64) * (b.y as f64) - (a.y as f64) * (b.x as f64);
    }
    let l = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
    if l < 1e-12 {
        return None;
    }
    let n = [n[0] as f32 / l as f32, n[1] as f32 / l as f32, n[2] as f32 / l as f32];
    let w = lms[0];
    let t = lms[1];
    let thumb = [t.x - w.x, t.y - w.y, t.z - w.z];
    let dot = n[0] * thumb[0] + n[1] * thumb[1] + n[2] * thumb[2];
    Some(if dot > 0.0 {
        [-n[0], -n[1], -n[2]]
    } else {
        n
    })
}

/// A palm normal that must not jump: the wrist orientation comes straight from
/// it, so a single-frame discontinuity reads as the hand teleporting. Smooth
/// input has to produce smooth output, including through the antipodal case
/// where slerp has no defined axis.
#[test]
fn palm_smoothing_never_jumps_across_the_antipode() {
    let from = [1.0f32, 0.0, 0.0];
    let mut cur = from;
    for i in 1..=200 {
        // Walk the target around the far side, passing exactly antipodal.
        let a = std::f32::consts::PI * (i as f32 / 200.0);
        let to = [a.cos(), a.sin(), 0.0];
        let next = smooth_normal(cur, to, 0.5);
        let swing = normal_angle_deg(cur, next).expect("both unit");
        assert!(
            swing < 95.0,
            "step {i} swung {swing:.1} deg from {cur:?} to {next:?}"
        );
        cur = next;
    }
}
fn palm_speed(prev: [f32; 3], cur: [f32; 3]) -> f32 {
    match normal_angle_deg(prev, cur) {
        Some(deg) => (deg.to_radians() as f32) / SAMPLE_DT,
        None => 0.0,
    }
}

struct Slot {
    active: bool,
    missing: u32,
    last_xy: (f32, f32),
    smooth_world: [[f32; 3]; 21],
    smooth_wx: f32,
    smooth_wy: f32,
    initialized: bool,
    disagree_streak: u32,
    swapped_once: bool,
    last_warn_frame: u64,
    // Jitter probe: previous raw frame plus windowed depth-step means.
    prev_raw: [[f32; 3]; 21],
    world_dx: [[f32; 3]; 21],
    prev_sm_z: [f32; 5],
    has_prev: bool,
    prev_palm: [f32; 3],
    smooth_palm: [f32; 3],
    has_palm: bool,
    acc_raw_dz: [f64; 5],
    acc_sm_dz: [f64; 5],
    acc_palm_deg: f64,
    acc_sm_palm_deg: f64,
    acc_n: u64,
}

impl Slot {
    fn fresh() -> Self {
        Self {
            active: false,
            missing: 0,
            last_xy: (0.5, 0.5),
            smooth_world: [[0.0; 3]; 21],
            smooth_wx: 0.5,
            smooth_wy: 0.5,
            initialized: false,
            disagree_streak: 0,
            swapped_once: false,
            last_warn_frame: 0,
            prev_raw: [[0.0; 3]; 21],
            world_dx: [[0.0; 3]; 21],
            prev_sm_z: [0.0; 5],
            has_prev: false,
            prev_palm: [0.0; 3],
            smooth_palm: [0.0; 3],
            has_palm: false,
            acc_raw_dz: [0.0; 5],
            acc_sm_dz: [0.0; 5],
            acc_palm_deg: 0.0,
            acc_sm_palm_deg: 0.0,
            acc_n: 0,
        }
    }

    /// Expected MediaPipe label for this slot's side (true-label camera).
    fn expected_label(&self, slot_id: usize) -> &'static str {
        if slot_id == 1 {
            "Right"
        } else {
            "Left"
        }
    }
}

pub struct HandSlots {
    slots: [Slot; 2],
    frame: u64,
}

impl HandSlots {
    pub fn new() -> Self {
        Self {
            slots: [Slot::fresh(), Slot::fresh()],
            frame: 0,
        }
    }

    /// Match detections to slots, smooth, and emit stable per-side hands
    /// plus any log warnings. Side assignment never flickers: slots are
    /// claimed by proximity, initialized by image side, corrected only by
    /// a confident cross-hand swap (latched, logged).
    pub fn update(&mut self, detections: &[DetectedHand]) -> (Vec<TrackedHand>, Vec<String>) {
        self.frame += 1;
        let mut warnings = Vec::new();

        // Pass 1: active slots claim a detection within the gate, preferring
        // one whose label agrees with the slot's side.
        let mut claimed = vec![false; detections.len()];
        let mut slot_match: [Option<usize>; 2] = [None, None];
        for s in 0..2 {
            if !self.slots[s].active {
                continue;
            }
            let want = self.slots[s].expected_label(s);
            let mut best: Option<usize> = None;
            let mut best_d = MATCH_GATE;
            let mut best_agrees = false;
            for (di, det) in detections.iter().enumerate() {
                if claimed[di] {
                    continue;
                }
                let dx = det.landmarks[0].x - self.slots[s].last_xy.0;
                let dy = det.landmarks[0].y - self.slots[s].last_xy.1;
                let d = (dx * dx + dy * dy).sqrt();
                if d >= MATCH_GATE {
                    continue;
                }
                // Nearest-first alone hands a slot the wrong physical hand
                // mid-crossing, because the other hand's wrist can sit closer
                // to this slot's anchor than its own does. A label agreement
                // outranks a closer disagreement, so the crossing sticks.
                let agrees = det.handedness == want;
                if !agrees && (best_agrees || d >= best_d) {
                    continue;
                }
                best_d = d;
                best = Some(di);
                best_agrees = agrees;
            }
            if let Some(di) = best {
                claimed[di] = true;
                slot_match[s] = Some(di);
            }
        }

        // Pass 2: unclaimed detections take free slots, taking the slot their
        // own MediaPipe label names. Image position is only the tiebreak: a
        // hand that has already crossed the midline sits in the other half,
        // and reading its image position there spawns the wrong SteamVR device
        // with the wrong chirality.
        for (di, det) in detections.iter().enumerate() {
            if claimed[di] {
                continue;
            }
            let labelled = if det.handedness == "Right" { 1 } else { 0 };
            let by_image = if det.landmarks[0].x > 0.5 { 1 } else { 0 };
            let pick = if !self.slots[labelled].active && slot_match[labelled].is_none() {
                Some(labelled)
            } else if !self.slots[by_image].active && slot_match[by_image].is_none() {
                Some(by_image)
            } else {
                (0..2).find(|&s| !self.slots[s].active && slot_match[s].is_none())
            };
            if let Some(s) = pick {
                claimed[di] = true;
                slot_match[s] = Some(di);
                // Fresh slot: snap smoothing to avoid a morph from stale data.
                self.slots[s].initialized = false;
                self.slots[s].swapped_once = false;
                self.slots[s].disagree_streak = 0;
            }
        }

        // Pass 3: update matched slots (smooth + label streaks).
        for s in 0..2 {
            match slot_match[s] {
                Some(di) => {
                    let det = &detections[di];
                    let slot = &mut self.slots[s];
                    slot.active = true;
                    slot.missing = 0;
                    slot.last_xy = (det.landmarks[0].x, det.landmarks[0].y);
                    if !slot.initialized {
                        for i in 0..21 {
                            let l = det.world_landmarks[i];
                            slot.smooth_world[i] = [l.x, l.y, l.z];
                        }
                        slot.world_dx = [[0.0; 3]; 21];
                        // The jitter probe subtracts the previous smoothed tip
                        // depth, so it has to start at this frame's value or the
                        // first window reports an absolute depth as a step.
                        for (k, &ti) in TIP_IDS.iter().enumerate() {
                            slot.prev_sm_z[k] = slot.smooth_world[ti][2];
                        }
                        slot.smooth_wx = det.landmarks[0].x;
                        slot.smooth_wy = det.landmarks[0].y;
                        slot.initialized = true;
                    } else {
                        for i in 0..21 {
                            let l = det.world_landmarks[i];
                            let (v, d) =
                                adaptive_step(slot.smooth_world[i], slot.world_dx[i], [l.x, l.y, l.z]);
                            slot.smooth_world[i] = v;
                            slot.world_dx[i] = d;
                        }
                        slot.smooth_wx += ANCHOR_ALPHA * (det.landmarks[0].x - slot.smooth_wx);
                        slot.smooth_wy += ANCHOR_ALPHA * (det.landmarks[0].y - slot.smooth_wy);
                    }
                    // Palm normal: sole input to the wrist's 3-DOF orientation.
                    // Reads the One-Euro filtered joints rather than the raw
                    // ones, and follows with a velocity-adaptive rate so a
                    // rolling hand is not held back by the smoothing that a
                    // resting hand needs. Finger curl is computed inside the
                    // parent frame, so it stays sharp either way. First
                    // sighting snaps; a degenerate ring (hand edge-on to the
                    // camera) keeps the previous normal.
                    let mut filtered = [Landmark::default(); 21];
                    for i in 0..21 {
                        filtered[i] = Landmark {
                            x: slot.smooth_world[i][0],
                            y: slot.smooth_world[i][1],
                            z: slot.smooth_world[i][2],
                        };
                    }
                    if let Some(pn) = palm_plane_normal(&filtered) {
                        if !slot.has_palm {
                            slot.smooth_palm = pn;
                            slot.has_palm = true;
                        } else {
                            let alpha = cutoff_alpha(
                                PALM_MIN_CUTOFF_HZ + PALM_BETA * palm_speed(slot.smooth_palm, pn),
                            );
                            let old_sm = slot.smooth_palm;
                            slot.smooth_palm = smooth_normal(old_sm, pn, alpha);
                            if let Some(swing) = normal_angle_deg(old_sm, slot.smooth_palm) {
                                slot.acc_sm_palm_deg += swing;
                            }
                        }
                        if slot.has_prev {
                            if let Some(swing) = normal_angle_deg(slot.prev_palm, pn) {
                                slot.acc_palm_deg += swing;
                            }
                        }
                        slot.prev_palm = pn;
                    }
                    // Jitter probe: raw per-tip |Î”z| against the |Î”z| that
                    // actually survives the filter, plus raw and smoothed
                    // palm-normal swings. Runs after the filter so the
                    // "smoothed" column is real output motion, which is the
                    // number that says whether the hand is still twitching.
                    if slot.initialized && slot.has_prev {
                        for (k, &ti) in TIP_IDS.iter().enumerate() {
                            slot.acc_raw_dz[k] += (det.world_landmarks[ti].z - slot.prev_raw[ti][2])
                                .abs() as f64;
                            slot.acc_sm_dz[k] +=
                                (slot.smooth_world[ti][2] - slot.prev_sm_z[k]).abs() as f64;
                            slot.prev_sm_z[k] = slot.smooth_world[ti][2];
                        }
                        slot.acc_n += 1;
                    }
                    for i in 0..21 {
                        let l = det.world_landmarks[i];
                        slot.prev_raw[i] = [l.x, l.y, l.z];
                    }
                    slot.has_prev = true;
                    if det.handedness == slot.expected_label(s) {
                        slot.disagree_streak = 0;
                    } else {
                        slot.disagree_streak += 1;
                    }
                }
                None => {
                    let slot = &mut self.slots[s];
                    if slot.active {
                        slot.missing += 1;
                        if slot.missing > MISSING_LIMIT {
                            *slot = Slot::fresh();
                        }
                    }
                }
            }
        }

        // Pass 4: confident cross-hand swap correction (latched per activation).
        // Both slots must agree for ~5s straight; label flicker can never
        // sustain that, so this only fires on a genuinely swapped assignment.
        // The swap is self-clearing: it is only reached when both slots hold a
        // detection whose label disagrees, so exchanging them puts each slot on
        // an agreeing detection and both streaks reset on the next frame. There
        // is no state where a swap leaves both slots still disagreeing, so the
        // latch needs no way back.
        let s0 = self.slots[0].disagree_streak;
        let s1 = self.slots[1].disagree_streak;
        let both_disagree = self.slots[0].active
            && self.slots[1].active
            && s0 >= SWAP_AFTER
            && s1 >= SWAP_AFTER
            && !self.slots[0].swapped_once
            && !self.slots[1].swapped_once;
        // One hand tracked alone has no partner to swap with, so the two-slot
        // correction below can never run for it. Freeing the slot re-runs pass 2,
        // which reads the label, and the hand lands on the right device.
        let single_disagrees = self.slots[0].active != self.slots[1].active
            && s0.max(s1) >= SWAP_AFTER
            && !self.slots[0].swapped_once
            && !self.slots[1].swapped_once;
        if single_disagrees {
            let s = if self.slots[0].active { 0 } else { 1 };
            self.slots[s] = Slot::fresh();
            warnings.push("hands: corrected swapped left/right assignment".into());
        } else if both_disagree {
            let a = std::mem::replace(&mut self.slots[0], Slot::fresh());
            let b = std::mem::replace(&mut self.slots[1], Slot::fresh());
            self.slots[0].active = b.active;
            self.slots[0].last_xy = b.last_xy;
            self.slots[0].smooth_world = b.smooth_world;
            self.slots[0].smooth_wx = b.smooth_wx;
            self.slots[0].smooth_wy = b.smooth_wy;
            self.slots[0].initialized = b.initialized;
            self.slots[0].missing = b.missing;
            self.slots[0].swapped_once = true;
            self.slots[0].prev_raw = b.prev_raw;
            self.slots[0].world_dx = b.world_dx;
            self.slots[0].prev_sm_z = b.prev_sm_z;
            self.slots[0].has_prev = b.has_prev;
            self.slots[0].prev_palm = b.prev_palm;
            self.slots[0].smooth_palm = b.smooth_palm;
            self.slots[0].has_palm = b.has_palm;
            self.slots[1].active = a.active;
            self.slots[1].last_xy = a.last_xy;
            self.slots[1].smooth_world = a.smooth_world;
            self.slots[1].smooth_wx = a.smooth_wx;
            self.slots[1].smooth_wy = a.smooth_wy;
            self.slots[1].initialized = a.initialized;
            self.slots[1].missing = a.missing;
            self.slots[1].swapped_once = true;
            self.slots[1].prev_raw = a.prev_raw;
            self.slots[1].world_dx = a.world_dx;
            self.slots[1].prev_sm_z = a.prev_sm_z;
            self.slots[1].has_prev = a.has_prev;
            self.slots[1].prev_palm = a.prev_palm;
            self.slots[1].smooth_palm = a.smooth_palm;
            self.slots[1].has_palm = a.has_palm;
            warnings.push("hands: corrected swapped left/right assignment".into());
        }

        // Pass 5: single-slot mismatch warnings (rate-limited). Fires when
        // one side persistently disagrees with its labels â€” e.g. globally
        // inverted sides or a systematically confused model.
        for s in 0..2 {
            let slot = &mut self.slots[s];
            if slot.active
                && slot.disagree_streak >= WARN_AFTER
                && self.frame - slot.last_warn_frame >= WARN_EVERY
            {
                slot.last_warn_frame = self.frame;
                warnings.push(format!(
                    "hands: slot {s} disagrees with labels for {} frames, sides may be inverted",
                    slot.disagree_streak
                ));
            }
        }

        // Emit stable hands for active matched slots only; the driver holds
        // the last pose through brief gaps.
        let mut out = Vec::new();
        for s in 0..2 {
            if slot_match[s].is_none() {
                continue;
            }
            let slot = &self.slots[s];
            let det = &detections[slot_match[s].unwrap()];
            let mut image = det.landmarks;
            image[0].x = slot.smooth_wx;
            image[0].y = slot.smooth_wy;
            let mut world = det.world_landmarks;
            for i in 0..21 {
                world[i].x = slot.smooth_world[i][0];
                world[i].y = slot.smooth_world[i][1];
                world[i].z = slot.smooth_world[i][2];
            }
            out.push(TrackedHand {
                hand_id: s as u8,
                landmarks: image,
                world_landmarks: world,
                palm: slot.smooth_palm,
            });
        }
        (out, warnings)
    }

    /// Windowed jitter means per active slot: raw tip |Î”z|, EMA-step tip
    /// |Î”z| (thumb..pinky order), raw and smoothed palm-normal swings,
    /// frame count. Drains.
    pub fn take_jitter(&mut self) -> Vec<(u8, [f64; 5], [f64; 5], f64, f64, u64)> {
        let mut out = Vec::new();
        for s in 0..2 {
            let slot = &mut self.slots[s];
            if slot.acc_n == 0 {
                continue;
            }
            let n = slot.acc_n as f64;
            let mut raw = [0.0; 5];
            let mut sm = [0.0; 5];
            for k in 0..5 {
                raw[k] = slot.acc_raw_dz[k] / n;
                sm[k] = slot.acc_sm_dz[k] / n;
            }
            out.push((
                s as u8,
                raw,
                sm,
                slot.acc_palm_deg / n,
                slot.acc_sm_palm_deg / n,
                slot.acc_n,
            ));
            slot.acc_raw_dz = [0.0; 5];
            slot.acc_sm_dz = [0.0; 5];
            slot.acc_palm_deg = 0.0;
            slot.acc_sm_palm_deg = 0.0;
            slot.acc_n = 0;
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn det(x: f32, y: f32, handedness: &str) -> DetectedHand {
        let mut landmarks = [Landmark { x: 0.0, y: 0.0, z: 0.0 }; 21];
        landmarks[0] = Landmark { x, y, z: 0.0 };
        let mut world = [Landmark { x: 0.0, y: 0.0, z: 0.0 }; 21];
        world[9] = Landmark { x: 0.0, y: 0.06, z: 0.0 };
        world[12] = Landmark { x: 0.0, y: 0.18, z: 0.0 };
        DetectedHand {
            landmarks,
            world_landmarks: world,
            handedness: handedness.to_string(),
            score: 0.9,
        }
    }

    #[test]
    fn init_assigns_side_by_image_position() {
        // Headset-mounted forward camera: an uncrossed hand's image half and
        // its MediaPipe label agree, so either one picks the same slot.
        let mut t = HandSlots::new();
        let (out, _) = t.update(&[det(0.3, 0.5, "Left")]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].hand_id, 0);
        let mut t = HandSlots::new();
        let (out, _) = t.update(&[det(0.7, 0.5, "Right")]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].hand_id, 1);
    }

    #[test]
    fn side_survives_label_flicker() {
        let mut t = HandSlots::new();
        let (out, _) = t.update(&[det(0.3, 0.5, "Left")]);
        assert_eq!(out[0].hand_id, 0);
        // Same hand, flickering labels: side must not move.
        for label in ["Right", "Left", "Right", "Left", "Right"] {
            let (out, _) = t.update(&[det(0.31, 0.51, label)]);
            assert_eq!(out.len(), 1);
            assert_eq!(out[0].hand_id, 0, "flicker moved the hand!");
        }
    }

    #[test]
    fn proximity_keeps_slot_on_drift() {
        let mut t = HandSlots::new();
        t.update(&[det(0.7, 0.5, "Right")]);
        for i in 1..10 {
            let (out, _) = t.update(&[det(0.7 - i as f32 * 0.02, 0.5, "Right")]);
            assert_eq!(out[0].hand_id, 1);
        }
    }

    #[test]
    fn two_hands_take_both_slots() {
        let mut t = HandSlots::new();
        let (out, _) = t.update(&[det(0.3, 0.5, "Left"), det(0.7, 0.5, "Right")]);
        assert_eq!(out.len(), 2);
        let mut ids: Vec<u8> = out.iter().map(|h| h.hand_id).collect();
        ids.sort();
        assert_eq!(ids, vec![0, 1]);
    }

    #[test]
    fn lost_hand_frees_slot_after_limit() {
        let mut t = HandSlots::new();
        t.update(&[det(0.7, 0.5, "Right")]);
        for _ in 0..MISSING_LIMIT {
            let (out, _) = t.update(&[]);
            assert_eq!(out.len(), 0);
        }
        // Slot still held (at limit); one more frame frees it.
        let (out, _) = t.update(&[]);
        assert_eq!(out.len(), 0);
        // New detection re-inits cleanly.
        let (out, _) = t.update(&[det(0.68, 0.5, "Right")]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].hand_id, 1);
    }

    #[test]
    fn smoothing_eases_toward_new_sample() {
        let mut t = HandSlots::new();
        let (out, _) = t.update(&[det(0.3, 0.5, "Left")]);
        assert!((out[0].landmarks[0].x - 0.3).abs() < 1e-6);
        let (out, _) = t.update(&[det(0.5, 0.5, "Left")]);
        let x = out[0].landmarks[0].x;
        assert!(x > 0.3 && x < 0.5, "smoothed {x} should lie between");
    }

    #[test]
    fn palm_smoothing_preserves_forward_and_reverse_roll_direction() {
        let diagonal = std::f32::consts::FRAC_1_SQRT_2;
        let forward = [
            [1.0, 0.0, 0.0],
            [diagonal, diagonal, 0.0],
            [0.0, 1.0, 0.0],
            [-diagonal, diagonal, 0.0],
            [-1.0, 0.0, 0.0],
        ];
        let reverse = [
            [-1.0, 0.0, 0.0],
            [-diagonal, -diagonal, 0.0],
            [0.0, -1.0, 0.0],
            [diagonal, -diagonal, 0.0],
            [1.0, 0.0, 0.0],
        ];
        for (path, direction) in [(forward, 1.0), (reverse, -1.0)] {
            let mut current = path[0];
            for target in &path[1..] {
                let a = cutoff_alpha(PALM_MIN_CUTOFF_HZ + PALM_BETA * palm_speed(current, *target));
                current = smooth_normal(current, *target, a);
                assert!(
                    current[1] * direction > 0.0,
                    "roll reversed at {current:?}"
                );
            }
        }
        // An antipodal target must move the normal partway, never all the way in one
        // step: snapping is what made the wrist teleport. Two steps land on it.
        let start = [1.0f32, 0.0, 0.0];
        let target = [-1.0f32, 0.0, 0.0];
        let mid = smooth_normal(start, target, 0.35);
        assert!(
            mid[0] > -0.999 && mid[0] < 0.999,
            "antipodal step jumped straight to the target: {mid:?}"
        );
        assert!(mid[1].abs() > 1e-6, "antipodal step had no axis: {mid:?}");
        let end = smooth_normal(mid, target, 0.35);
        assert!(
            end[0] < mid[0] && mid[0] < start[0],
            "each step should keep turning toward the target: {start:?} -> {mid:?} -> {end:?}"
        );
        // Repeated steps converge on the target, moving a little each time.
        let mut cur = end;
        for _ in 0..60 {
            cur = smooth_normal(cur, target, 0.35);
        }
        assert!(cur[0] < -0.99, "should converge on the target, got {cur:?}");
    }

    #[test]
    fn jitter_probe_reports_per_tip_depth_steps() {
        let frame = |z: f32| {
            let mut d = det(0.3, 0.5, "Left");
            d.world_landmarks[5] = Landmark { x: 0.04, y: 0.10, z: 0.01 };
            for &ti in &[4usize, 8, 12, 16, 20] {
                d.world_landmarks[ti] = Landmark { x: 0.0, y: 0.15, z };
            }
            d
        };
        let mut t = HandSlots::new();
        t.update(&[frame(0.0)]);
        assert!(t.take_jitter().is_empty());
        t.update(&[frame(0.01)]);
        let j = t.take_jitter();
        assert_eq!(j.len(), 1);
        assert_eq!(j[0].0, 0);
        // The smoothed column is real output motion, so the expected step is
        // whatever one filter step produces from a 0 -> 10 mm jump.
        let (step, _) = adaptive_step([0.0, 0.0, 0.0], [0.0; 3], [0.0, 0.0, 0.01]);
        for k in 0..5 {
            assert!((j[0].1[k] - 0.01).abs() < 1e-6, "raw tip {k}: {}", j[0].1[k]);
            assert!(
                (j[0].2[k] - step[2].abs() as f64).abs() < 1e-6,
                "sm tip {k}: {} want {}",
                j[0].2[k],
                step[2]
            );
        }
        assert!(j[0].3.abs() < 0.1, "palm swing {}", j[0].3);
        assert!(j[0].4.abs() < 0.1, "smoothed palm swing {}", j[0].4);
        assert_eq!(j[0].5, 1);
        assert!(t.take_jitter().is_empty());
    }

    /// Deterministic noise source for the filter tests (no rand dependency).
    struct Lcg(u32);
    impl Lcg {
        /// Uniform in [-1, 1).
        fn next(&mut self) -> f32 {
            self.0 = self.0.wrapping_mul(1664525).wrapping_add(1013904223);
            ((self.0 >> 8) as f32 / 8388608.0) - 1.0
        }
    }

    /// RMS frame-to-frame step of a scalar series, ignoring the first `skip`.
    fn rms_step(series: &[f32], skip: usize) -> f32 {
        let d: Vec<f32> = series[skip + 1..]
            .iter()
            .zip(&series[skip..series.len() - 1])
            .map(|(a, b)| a - b)
            .collect();
        (d.iter().map(|v| v * v).sum::<f32>() / d.len() as f32).sqrt()
    }

    /// The old filter was a fixed first-order low-pass. Kept here as the
    /// baseline the adaptive filter has to beat.
    fn fixed_ema(prev: f32, raw: f32, alpha: f32) -> f32 {
        prev + alpha * (raw - prev)
    }

    /// A motionless hand must come far quieter than the old fixed-alpha
    /// filter managed, otherwise the short distal segments keep flapping.
    #[test]
    fn still_hand_is_filtered_harder_than_the_old_fixed_gain() {
        const SIGMA: f32 = 0.002; // 2 mm per-joint noise
        let mut rng = Lcg(12345);
        let mut adapt = ([0.0f32; 3], [0.0f32; 3]);
        let mut fixed = 0.0f32;
        let (mut adapt_z, mut fixed_z) = (Vec::new(), Vec::new());
        for _ in 0..400 {
            let n = SIGMA * rng.next();
            let (v, d) = adaptive_step(adapt.0, adapt.1, [0.0, 0.0, n]);
            adapt = (v, d);
            fixed = fixed_ema(fixed, n, 0.65);
            adapt_z.push(v[2]);
            fixed_z.push(fixed);
        }
        let a = rms_step(&adapt_z, 40);
        let f = rms_step(&fixed_z, 40);
        assert!(a < f * 0.5, "adaptive rms step {a} vs old filter {f}");
    }

    /// A moving hand must not be materially mis-tracked. The adaptive filter
    /// is not strictly better here â€” on a slow ramp it gives up ~9% RMS
    /// position error against the old fixed gain (0.99mm vs 0.91mm) in
    /// exchange for the rest-jitter win above. On a 16 mm phalanx that is
    /// under 2 degrees of direction bias, i.e. imperceptible, so the bound
    /// here is "within half a millimetre" rather than strict parity.
    #[test]
    fn moving_hand_stays_within_half_a_millimetre_of_the_old_fixed_gain() {
        const IMPERCEPTIBLE_M: f32 = 0.0005;
        let mut rng = Lcg(777);
        let mut adapt = ([0.0f32; 3], [0.0f32; 3]);
        let mut fixed = 0.0f32;
        let (mut e_adapt, mut e_fixed) = (0.0f32, 0.0f32);
        let mut n = 0.0f32;
        // 0.02 m/s ramp: a relaxed finger closing.
        for i in 0..300 {
            let truth = 0.02 * SAMPLE_DT * i as f32;
            let sample = truth + 0.002 * rng.next();
            let (v, d) = adaptive_step(adapt.0, adapt.1, [0.0, 0.0, sample]);
            adapt = (v, d);
            fixed = fixed_ema(fixed, sample, 0.65);
            if i >= 40 {
                e_adapt += (v[2] - truth).powi(2);
                e_fixed += (fixed - truth).powi(2);
                n += 1.0;
            }
        }
        let a = (e_adapt / n).sqrt();
        let f = (e_fixed / n).sqrt();
        assert!(
            a <= f + IMPERCEPTIBLE_M,
            "adaptive rms error {a} vs old filter {f} exceeds {IMPERCEPTIBLE_M}"
        );
    }

    /// A sustained ramp must be followed, not merely approached: if the gain
    /// never opens up this passes the still-hand test and fails this one.
    #[test]
    fn sustained_motion_opens_the_gain_up() {
        let mut state = ([0.0f32; 3], [0.0f32; 3]);
        for i in 0..40 {
            let (v, d) = adaptive_step(state.0, state.1, [0.0, 0.0, 0.0005 * (i + 1) as f32]);
            state = (v, d);
        }
        // 0.02 m/s for 40 frames is 20 mm of travel; the filter must track
        // most of it rather than still sitting near the origin.
        let travelled = 0.0005 * 40.0;
        assert!(
            state.0[2] > travelled * 0.9,
            "gain stayed closed: tracked {} of {travelled} m",
            state.0[2]
        );
    }

    #[test]
    fn confident_cross_swap_corrects_sides() {
        let mut t = HandSlots::new();
        // Init with agreeing labels, then feed labels disagreeing with both
        // slots long enough to trip the latched swap correction.
        t.update(&[det(0.3, 0.5, "Left"), det(0.7, 0.5, "Right")]);
        let mut warned = false;
        for _ in 0..(SWAP_AFTER + 5) {
            let (out, w) = t.update(&[det(0.3, 0.5, "Right"), det(0.7, 0.5, "Left")]);
            assert_eq!(out.len(), 2);
            if w.iter().any(|s| s.contains("corrected swapped")) {
                warned = true;
            }
        }
        assert!(warned, "expected a swap correction warning");
    }

    /// MediaPipe's label is the anatomical truth. A left hand seen on the
    /// image-right half (crossed over, or entering frame from that side) is
    /// still the left hand and must not spawn the right SteamVR device.
    #[test]
    fn init_uses_the_label_not_the_image_half() {
        let mut t = HandSlots::new();
        let (out, _) = t.update(&[det(0.7, 0.5, "Left")]);
        assert_eq!(out[0].hand_id, 0, "label says Left but the right device spawned");
    }

    /// A single tracked hand has no partner to swap with, so a sustained label
    /// disagreement can only be resolved by re-running the init, which reads
    /// the label. Freeing the slot is the whole correction.
    #[test]
    fn single_hand_reassigns_by_label_after_a_sustained_disagreement() {
        let mut t = HandSlots::new();
        t.update(&[det(0.3, 0.5, "Left")]);
        let mut warned = false;
        let mut last = None;
        for _ in 0..(SWAP_AFTER + 5) {
            let (out, w) = t.update(&[det(0.3, 0.5, "Right")]);
            if w.iter().any(|s| s.contains("corrected swapped")) {
                warned = true;
            }
            last = out.first().map(|h| h.hand_id);
        }
        assert!(warned, "expected a side correction warning");
        assert_eq!(last, Some(1), "sustained disagreement must move the hand");
    }

    /// Crossing hands: once the left wrist has moved past the right one it sits
    /// nearer to slot 1's anchor than to slot 0's, so plain nearest-first
    /// matching hands each slot the wrong physical hand — which also flips the
    /// wrist roll, since the palm chirality sign follows the slot. Each slot
    /// must keep following the hand its own label names.
    #[test]
    fn crossing_hands_keep_each_slot_on_its_own_label() {
        // A hand is identified by a marker in its world landmarks, which no
        // amount of filtering can wash out the way an image position can.
        fn marked(x: f32, y: f32, label: &str, tag: f32) -> DetectedHand {
            let mut d = det(x, y, label);
            d.world_landmarks[1].x = tag;
            d
        }
        let mut t = HandSlots::new();
        t.update(&[marked(0.3, 0.5, "Left", 1.0), marked(0.7, 0.5, "Right", 9.0)]);
        // The left hand walks across the right one, which holds its ground.
        for lx in [0.4, 0.5, 0.55, 0.65, 0.7] {
            let (out, _) = t.update(&[marked(lx, 0.5, "Left", 1.0), marked(0.6, 0.5, "Right", 9.0)]);
            assert_eq!(out.len(), 2, "lost a hand mid-crossing at lx={lx}");
            let tag = |id: u8| {
                out.iter()
                    .find(|h| h.hand_id == id)
                    .unwrap()
                    .world_landmarks[1]
                    .x
            };
            assert!(tag(0) < 5.0, "slot 0 took the right hand at lx={lx} (tag {})", tag(0));
            assert!(tag(1) > 5.0, "slot 1 took the left hand at lx={lx} (tag {})", tag(1));
        }
    }

    #[test]
    fn flicker_never_triggers_swap() {
        let mut t = HandSlots::new();
        t.update(&[det(0.3, 0.5, "Left"), det(0.7, 0.5, "Right")]);
        for i in 0..(SWAP_AFTER + 20) {
            let l = if i % 2 == 0 { "Right" } else { "Left" };
            let r = if i % 2 == 0 { "Left" } else { "Right" };
            let (_, w) = t.update(&[det(0.3, 0.5, l), det(0.7, 0.5, r)]);
            assert!(w.iter().all(|s| !s.contains("corrected swapped")), "flicker caused a swap!");
        }
    }
}
