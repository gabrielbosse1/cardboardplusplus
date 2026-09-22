use crate::net::mediapipe::{DetectedHand, Landmark};

/// Stable 2-slot hand tracking across camera frames.
///
/// MediaPipe's per-frame handedness label flickers (especially on dorsal
/// views), which used to teleport skeletons between SteamVR devices and
/// flip the frame chirality mid-track (inside-out fingers, inverted
/// curls). Slots fix the side once and track by wrist-position continuity;
/// the label only feeds a debounce streak that detects a persistently
/// swapped assignment using both hands' histories.
///
/// Slot index == SteamVR device id (0 = left, 1 = right) and never changes
/// while tracked. A hand keeps its slot (and side) across crossings, brief
/// losses, and label flicker.
pub struct TrackedHand {
    pub hand_id: u8,
    pub landmarks: [Landmark; 21],
    pub world_landmarks: [Landmark; 21],
}

/// Wrist travel per frame (normalized image units) allowed to keep a slot.
const MATCH_GATE: f32 = 0.35;
/// Frames without a sighting before a slot is freed.
const MISSING_LIMIT: u32 = 20;
/// Smoothing factors (weight of the new sample).
const ANCHOR_ALPHA: f32 = 0.5;
const JOINT_ALPHA: f32 = 0.65;
/// Consecutive disagreeing labels on BOTH slots before correcting a swap.
const SWAP_AFTER: u32 = 90;
/// Consecutive disagreeing labels before warning about a single slot.
const WARN_AFTER: u32 = 150;
/// Minimum frames between repeat warnings.
const WARN_EVERY: u64 = 1200;

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

        // Pass 1: active slots claim their nearest detection within the gate.
        let mut claimed = vec![false; detections.len()];
        let mut slot_match: [Option<usize>; 2] = [None, None];
        for s in 0..2 {
            if !self.slots[s].active {
                continue;
            }
            let mut best: Option<usize> = None;
            let mut best_d = MATCH_GATE;
            for (di, det) in detections.iter().enumerate() {
                if claimed[di] {
                    continue;
                }
                let dx = det.landmarks[0].x - self.slots[s].last_xy.0;
                let dy = det.landmarks[0].y - self.slots[s].last_xy.1;
                let d = (dx * dx + dy * dy).sqrt();
                if d < best_d {
                    best_d = d;
                    best = Some(di);
                }
            }
            if let Some(di) = best {
                claimed[di] = true;
                slot_match[s] = Some(di);
            }
        }

        // Pass 2: unclaimed detections take free slots. Headset-mounted
        // geometry (rear camera faces forward, unmirrored): the user's left
        // hand appears on the image-LEFT half, so image-left initializes
        // slot 0 (left device) and image-right slot 1.
        for (di, det) in detections.iter().enumerate() {
            if claimed[di] {
                continue;
            }
            let want = if det.landmarks[0].x > 0.5 { 1 } else { 0 };
            let pick = if !self.slots[want].active && slot_match[want].is_none() {
                Some(want)
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
                        slot.smooth_wx = det.landmarks[0].x;
                        slot.smooth_wy = det.landmarks[0].y;
                        slot.initialized = true;
                    } else {
                        for i in 0..21 {
                            let l = det.world_landmarks[i];
                            let sm = &mut slot.smooth_world[i];
                            sm[0] += JOINT_ALPHA * (l.x - sm[0]);
                            sm[1] += JOINT_ALPHA * (l.y - sm[1]);
                            sm[2] += JOINT_ALPHA * (l.z - sm[2]);
                        }
                        slot.smooth_wx += ANCHOR_ALPHA * (det.landmarks[0].x - slot.smooth_wx);
                        slot.smooth_wy += ANCHOR_ALPHA * (det.landmarks[0].y - slot.smooth_wy);
                    }
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
        let s0 = self.slots[0].disagree_streak;
        let s1 = self.slots[1].disagree_streak;
        if self.slots[0].active
            && self.slots[1].active
            && s0 >= SWAP_AFTER
            && s1 >= SWAP_AFTER
            && !self.slots[0].swapped_once
            && !self.slots[1].swapped_once
        {
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
            self.slots[1].active = a.active;
            self.slots[1].last_xy = a.last_xy;
            self.slots[1].smooth_world = a.smooth_world;
            self.slots[1].smooth_wx = a.smooth_wx;
            self.slots[1].smooth_wy = a.smooth_wy;
            self.slots[1].initialized = a.initialized;
            self.slots[1].missing = a.missing;
            self.slots[1].swapped_once = true;
            warnings.push("hands: corrected swapped left/right assignment".into());
        }

        // Pass 5: single-slot mismatch warnings (rate-limited). Fires when
        // one side persistently disagrees with its labels — e.g. globally
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
            });
        }
        (out, warnings)
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
        // Headset-mounted forward camera: image-left holds the user's left hand.
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
