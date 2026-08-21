//! Draw hand skeleton (landmarks + connections) directly onto RGBA pixels.
//!
//! The MediaPipe hand model defines 21 landmarks per hand with standard
//! connections. This module renders them onto the camera frame so the
//! Slint viewer shows a live overlay without needing a custom canvas.

use crate::net::mediapipe::DetectedHand;

/// MediaPipe hand skeleton connections: pairs of landmark indices.
const CONNECTIONS: &[[usize; 2]] = &[
    [0, 1], [1, 2], [2, 3], [3, 4],       // thumb
    [0, 5], [5, 6], [6, 7], [7, 8],       // index
    [5, 9], [9, 10], [10, 11], [11, 12],  // middle
    [9, 13], [13, 14], [14, 15], [15, 16],// ring
    [13, 17], [17, 18], [18, 19], [19, 20],// pinky
    [0, 17],                                // palm
];

/// Draw all detected hands onto the RGBA buffer in-place.
pub fn draw_hands(rgba: &mut [u8], w: u32, h: u32, hands: &[DetectedHand]) {
    for hand in hands {
        // Draw connections (lines).
        for &[a, b] in CONNECTIONS {
            let ax = (hand.landmarks[a].x * w as f32) as i32;
            let ay = (hand.landmarks[a].y * h as f32) as i32;
            let bx = (hand.landmarks[b].x * w as f32) as i32;
            let by = (hand.landmarks[b].y * h as f32) as i32;
            draw_line(rgba, w, h, ax, ay, bx, by, [0, 255, 120, 220]);
        }
        // Draw landmark dots.
        for lm in &hand.landmarks {
            let x = (lm.x * w as f32) as i32;
            let y = (lm.y * h as f32) as i32;
            draw_dot(rgba, w, h, x, y, 3, [255, 255, 255, 240]);
        }
    }
}

/// Set a single pixel if in bounds.
fn put_pixel(rgba: &mut [u8], w: u32, h: u32, x: i32, y: i32, c: [u8; 4]) {
    if x >= 0 && y >= 0 && (x as u32) < w && (y as u32) < h {
        let off = ((y as u32 * w + x as u32) * 4) as usize;
        if off + 3 < rgba.len() {
            rgba[off] = c[0];
            rgba[off + 1] = c[1];
            rgba[off + 2] = c[2];
            rgba[off + 3] = c[3];
        }
    }
}

/// Draw a filled circle (dot) at (cx, cy).
fn draw_dot(rgba: &mut [u8], w: u32, h: u32, cx: i32, cy: i32, r: i32, c: [u8; 4]) {
    for dy in -r..=r {
        for dx in -r..=r {
            if dx * dx + dy * dy <= r * r {
                put_pixel(rgba, w, h, cx + dx, cy + dy, c);
            }
        }
    }
}

/// Draw a line using Bresenham's algorithm.
fn draw_line(rgba: &mut [u8], w: u32, h: u32, mut x0: i32, mut y0: i32, x1: i32, y1: i32, c: [u8; 4]) {
    let dx = (x1 - x0).abs();
    let dy = -(y1 - y0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut err = dx + dy;

    loop {
        put_pixel(rgba, w, h, x0, y0, c);
        if x0 == x1 && y0 == y1 { break; }
        let e2 = 2 * err;
        if e2 >= dy {
            err += dy;
            x0 += sx;
        }
        if e2 <= dx {
            err += dx;
            y0 += sy;
        }
    }
}
