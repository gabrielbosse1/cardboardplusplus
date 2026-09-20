//! Shared-memory transport between the SteamVR driver and the bridge.
//! `mem` owns the OS mapping, `protocol` defines the layout, `ring` reads/writes it.
pub mod mem;
pub mod protocol;
pub mod ring;
pub use mem::MemError;
pub use protocol::{BridgeMessage, CMD_REGION_SIZE, MsgType, payload};
pub use ring::{BridgeConsumer, CmdProducer};
