//! Bridge service library: shared app state (`app`), UDP/TCP plane (`net`),
//! and the camera hand skeleton painter (`hand_overlay`).
pub mod app;
pub mod net;
pub mod hand_overlay;
pub(crate) use app::debug_log;
