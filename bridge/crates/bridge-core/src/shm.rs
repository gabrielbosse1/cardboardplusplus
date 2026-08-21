//! Shared-memory consumer service + settings channel.
//!
//! Wraps `bridge-shm` in a small ergonomic layer and logs telemetry about the
//! traffic so the UI can show liveness without touching protocol internals.
//!
//! Two regions are involved:
//! - the **status region** (driver → bridge): telemetry, frame submits, etc.,
//!   consumed here via `ShmService`;
//! - the **command region** (bridge → driver): stream settings, produced here
//!   via `SettingsChannel`.

use bridge_shm::{
    BridgeConsumer, BridgeMessage, CMD_REGION_SIZE, MemError, CmdProducer, payload,
};

pub struct ShmService {
    consumer: BridgeConsumer,
    pub last_write_seq: u64,
    pub dropped_total: u64,
    pub msgs_total: u64,
}

impl ShmService {
    pub fn open(size: usize) -> Result<Self, MemError> {
        let consumer = BridgeConsumer::open(size)?;
        Ok(Self {
            consumer,
            last_write_seq: 0,
            dropped_total: 0,
            msgs_total: 0,
        })
    }

    pub fn dropped(&self) -> u64 {
        self.consumer.dropped()
    }

    pub fn pending(&self) -> u64 {
        self.consumer.pending()
    }

    /// Drain everything currently available, returning each parsed message.
    pub fn drain(&mut self) -> Vec<BridgeMessage> {
        let mut out = Vec::new();
        while let Some(msg) = self.consumer.try_consume() {
            self.msgs_total += 1;
            out.push(msg);
        }
        self.last_write_seq = self.consumer.write_seq();
        self.dropped_total += self.consumer.dropped();
        out
    }
}

/// Bridge → driver settings push over the command region.
pub struct SettingsChannel {
    producer: CmdProducer,
}

impl SettingsChannel {
    /// Create or open the command region the driver polls for settings.
    pub fn open() -> Result<Self, MemError> {
        Ok(Self {
            producer: CmdProducer::open(CMD_REGION_SIZE)?,
        })
    }

    /// Push the current stream settings as one latest-wins message.
    pub fn push(
        &mut self,
        width: u32,
        height: u32,
        fps: u32,
        bitrate_kbps: u32,
        encoder: u32,
        stream_enabled: u32,
    ) -> u64 {
        let mut s = payload::SettingsChange {
            width,
            height,
            fps,
            bitrate_kbps,
            encoder,
            stream_enabled,
            seq: 0,
        };
        // seq mirrors the ring turn so the driver can detect generations.
        s.seq = self.producer.write_seq() + 1;
        self.producer.publish_settings(&s)
    }
}

/// Short human-readable tag for the UI status pane / logging.
pub fn msg_tag(msg: &BridgeMessage) -> &'static str {
    match msg {
        BridgeMessage::TextureSetCreated(_) => "texture_set_created",
        BridgeMessage::FrameSubmitted(_) => "frame_submitted",
        BridgeMessage::CapReported(_) => "cap_reported",
        BridgeMessage::Pose(_) => "pose",
        BridgeMessage::ControllerInput(_) => "controller_input",
        BridgeMessage::Telemetry(_) => "telemetry",
        BridgeMessage::Unknown { .. } => "unknown",
    }
}