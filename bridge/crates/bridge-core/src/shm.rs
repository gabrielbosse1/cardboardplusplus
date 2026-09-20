use bridge_shm::{
    BridgeConsumer, BridgeMessage, CMD_REGION_SIZE, MemError, CmdProducer, payload,
};
/// Read side of the driver status ring with cumulative counters for the
/// diagnostics tab. Opened once at bridge startup; drain() is polled.
pub struct ShmService {
    consumer: BridgeConsumer,
    pub last_write_seq: u64,
    pub dropped_total: u64,
    pub msgs_total: u64,
    dropped_seen: u64,
}
impl ShmService {
    /// Opens the driver region of `size` bytes. Fails when SteamVR never
    /// started the driver (no mapping yet) or the layout version mismatches.
    pub fn open(size: usize) -> Result<Self, MemError> {
        let consumer = BridgeConsumer::open(size)?;
        Ok(Self {
            consumer,
            last_write_seq: 0,
            dropped_total: 0,
            msgs_total: 0,
            dropped_seen: 0,
        })
    }
    /// Messages the consumer skipped by falling behind. Feeds the dropped
    /// counter in diagnostics.
    pub fn dropped(&self) -> u64 {
        self.consumer.dropped()
    }
    /// Messages waiting unread. Polled to size each drain batch.
    pub fn pending(&self) -> u64 {
        self.consumer.pending()
    }
    /// Pulls every queued message and refreshes the cumulative totals.
    /// Called on each UI tick; returns the batch for the caller to apply.
    pub fn drain(&mut self) -> Vec<BridgeMessage> {
        let mut out = Vec::new();
        while let Some(msg) = self.consumer.try_consume() {
            self.msgs_total += 1;
            out.push(msg);
        }
        self.last_write_seq = self.consumer.write_seq();
        let cur = self.consumer.dropped();
        self.dropped_total += cur.saturating_sub(self.dropped_seen);
        self.dropped_seen = cur;
        out
    }
}
/// Write side of the command ring: pushes Stream-tab settings to the driver.
/// Latest-wins — the driver only honors the highest `seq` it has seen.
pub struct SettingsChannel {
    producer: CmdProducer,
}
impl SettingsChannel {
    /// Creates (or attaches to) the command region. Called once at startup
    /// alongside ShmService::open.
    pub fn open() -> Result<Self, MemError> {
        Ok(Self {
            producer: CmdProducer::open(CMD_REGION_SIZE)?,
        })
    }
    /// Publishes one settings snapshot; `encoder` is the numeric EncoderChoice
    /// id, `stream_enabled` gates the driver emit loop. Returns the sequence
    /// number so callers can confirm the driver consumed it.
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
        s.seq = self.producer.write_seq() + 1;
        self.producer.publish_settings(&s)
    }
}
/// Short log/DB tag for a message variant. Used when draining batches into
/// the ring log and diagnostics counters.
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
