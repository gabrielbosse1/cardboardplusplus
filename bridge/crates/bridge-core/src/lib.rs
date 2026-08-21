//! Bridge core library.
//!
//! This crate is deliberately thin right now: the first migrated function is
//! the **swap-texture + Present path**, so the core owns
//!
//! 1. consuming the shared ring (`shm` module) written by the driver, and
//! 2. acquiring the raw D3D11 shared textures referenced by the forwarded
//!    handles (`d3d11` module) and reading them back to CPU memory.
//!
//! Encoding, network, settings and preview will land here function-by-function;
//! see `bridge/docs/MIGRATION_STATUS.md` for the order.

pub mod d3d11;
pub mod shm;