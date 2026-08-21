//! # bridge-shm
//!
//! Shared-memory transport between the SteamVR driver and the Rust bridge.
//!
//! **Roles**
//! - The **driver** (C++, inside SteamVR's process) creates the region, acts as
//!   the *producer*, writes fixed-size slots into a ring, then publishes a
//!   monotonic write counter in the header.
//! - The **bridge** (this crate) opens the same named region, acts as the
//!   *consumer*, reads slots from its own read cursor up to the published write
//!   counter, and hands the parsed messages to the caller.
//!
//! The on-wire layout is defined in `protocol.rs` and **must stay byte-for-byte
//! identical** with `driver_cardboardplusplus/include/BridgeProtocol.h`. See
//! `bridge/docs/TRANSPORT.md` for the authoritative spec.
//!
//! **Platforms**
//! - Windows: named file mapping `Local\CardboardPPBridge` via
//!   `CreateFileMappingW` / `MapViewOfFile`.
//! - Linux: POSIX shared memory `/cardboard_pp_bridge` via `shm_open`/`mmap`.
//!   The Linux backend compiles today but is only exercised when the (yet
//!   unimplemented) Linux driver lands.
//!
//! Transport is single-producer / single-consumer; no locks are used. The
//! producer writes a slot *then* bumps the header counter, so the consumer can
//! never observe a torn slot.

pub mod mem;
pub mod protocol;
pub mod ring;

pub use mem::MemError;
pub use protocol::{BridgeMessage, CMD_REGION_SIZE, MsgType, payload};
pub use ring::{BridgeConsumer, CmdProducer};