//! Bridge core library.
//!
//! Shared code for the bridge binaries: the shared-memory consumer (`shm`
//! module) and portable default paths (`paths` module).

pub mod driver_deps;
pub mod paths;
pub mod shm;