// Phone -> bridge telemetry wire (UDP 42071). Binary packets are tag-prefixed
// little-endian frames; the text hello is the only variable-length message.
#[derive(Debug, Clone, Copy, Default)]
#[allow(dead_code)]
pub struct GyroSample {
    pub timestamp_ms: u64,
    pub angular_velocity: [f32; 3],
    pub acceleration: [f32; 3],
    pub magnetic_field: [f32; 3],
}
/// On-phone hand-presence hint (tag 0x11). The real landmarks come from the
/// MediaPipe sidecar; this just tells the bridge whether hands are visible.
#[derive(Debug, Clone, Copy, Default)]
#[allow(dead_code)]
pub struct HandFrame {
    pub timestamp_ms: u64,
    pub hands: u8,
    pub landmarks_per_hand: u8,
    pub confidence: f32,
}
/// Fused orientation quat [w, x, y, z] (tag 0x12). This is the head-tracking
/// path: the bridge forwards each sample to the driver on UDP 42074.
#[derive(Debug, Clone, Copy, Default)]
pub struct RotationSample {
    pub timestamp_ms: u64,
    pub quat: [f32; 4],
}
/// Phone decode health (tag 0x13, ~every 2s). Drives the adaptive-bitrate
/// decision: rising `stalls` means the driver should lower the bitrate.
#[derive(Debug, Clone, Copy, Default)]
#[allow(dead_code)]
pub struct NetStats {
    pub timestamp_ms: u64,
    pub frames_decoded: u32,
    pub stalls: u32,
    pub decoded_fps: f32,
}
/// One decoded datagram from the telemetry socket.
#[derive(Debug, Clone)]
pub enum TelemetryPacket {
    Gyro(GyroSample),
    Hand(HandFrame),
    Rotation(RotationSample),
    NetStats(NetStats),
    Hello(String),
    Ping,
    Unknown,
}
const GYRO_PACKET_LEN: usize = 45;
const HAND_PACKET_LEN: usize = 15;
const ROTATION_PACKET_LEN: usize = 25;
const NET_STATS_PACKET_LEN: usize = 21;
/// Decodes one telemetry datagram by its leading tag byte. Short buffers fall
/// through to Unknown (callers drop them); the text hello is tried last.
pub fn parse_packet(buf: &[u8]) -> TelemetryPacket {
    if buf.is_empty() {
        return TelemetryPacket::Unknown;
    }
    match buf[0] {
        0x10 if buf.len() >= GYRO_PACKET_LEN => TelemetryPacket::Gyro(parse_gyro(buf)),
        0x11 if buf.len() >= HAND_PACKET_LEN => TelemetryPacket::Hand(parse_hand(buf)),
        0x12 if buf.len() >= ROTATION_PACKET_LEN => TelemetryPacket::Rotation(parse_rotation(buf)),
        0x13 if buf.len() >= NET_STATS_PACKET_LEN => TelemetryPacket::NetStats(parse_net_stats(buf)),
        0x20 => TelemetryPacket::Ping,
        _ => {
            if let Some(version) = phone_hello_version(buf) {
                TelemetryPacket::Hello(version)
            } else {
                TelemetryPacket::Unknown
            }
        }
    }
}
/// Tag 0x10 layout: u64 timestamp, then 9xf32 (gyro xyz, accel xyz, mag xyz).
fn parse_gyro(buf: &[u8]) -> GyroSample {
    GyroSample {
        timestamp_ms: read_u64(&buf[1..9]),
        angular_velocity: [read_f32(buf, 9), read_f32(buf, 13), read_f32(buf, 17)],
        acceleration: [read_f32(buf, 21), read_f32(buf, 25), read_f32(buf, 29)],
        magnetic_field: [read_f32(buf, 33), read_f32(buf, 37), read_f32(buf, 41)],
    }
}
/// Tag 0x11 layout: u64 timestamp, u8 hands, u8 landmarks, f32 confidence.
fn parse_hand(buf: &[u8]) -> HandFrame {
    HandFrame {
        timestamp_ms: read_u64(&buf[1..9]),
        hands: buf[9],
        landmarks_per_hand: buf[10],
        confidence: read_f32(buf, 11),
    }
}
/// Tag 0x12 layout: u64 timestamp, then 4xf32 quat [w, x, y, z].
fn parse_rotation(buf: &[u8]) -> RotationSample {
    RotationSample {
        timestamp_ms: read_u64(&buf[1..9]),
        quat: [read_f32(buf, 9), read_f32(buf, 13), read_f32(buf, 17), read_f32(buf, 21)],
    }
}
/// Tag 0x13 layout: u64 timestamp, u32 frames, u32 stalls, f32 fps.
fn parse_net_stats(buf: &[u8]) -> NetStats {
    NetStats {
        timestamp_ms: read_u64(&buf[1..9]),
        frames_decoded: read_u32(buf, 9),
        stalls: read_u32(buf, 13),
        decoded_fps: read_f32(buf, 17),
    }
}
/// Extracts the build-commit suffix from "CARDBOARD_PHONE_HELLO vN [commit]".
/// Returns None when the buffer is not a phone hello at all.
pub fn phone_hello_version(buf: &[u8]) -> Option<String> {
    let text = String::from_utf8_lossy(buf);
    let rest = text.trim_start().strip_prefix("CARDBOARD_PHONE_HELLO")?;
    let rest = rest.trim_start();
    if rest.is_empty() {
        return Some(String::new());
    }
    let mut toks = rest.split_whitespace();
    toks.next()?;
    Some(toks.next().unwrap_or("").to_string())
}
/// Little-endian field readers. Callers guarantee lengths via the
/// GYRO/HAND/ROTATION/NET_STATS length checks in parse_packet, so a short
/// slice here is a caller error and panics loudly.
fn read_u64(buf: &[u8]) -> u64 {
    u64::from_le_bytes(buf.try_into().expect("fixed-size u64 slice"))
}
/// Reads one LE f32 at `offset` (bounds pre-checked by parse_packet).
fn read_f32(buf: &[u8], offset: usize) -> f32 {
    f32::from_le_bytes(buf[offset..offset + 4].try_into().expect("fixed-size f32 slice"))
}
/// Reads one LE u32 at `offset` (bounds pre-checked by parse_packet).
fn read_u32(buf: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(buf[offset..offset + 4].try_into().expect("fixed-size u32 slice"))
}
#[cfg(test)]
mod tests {
    // Contract tests: wire-builder helpers construct raw packets byte-for-byte
    // (mirroring TelemetrySender.java) and each test asserts parse_packet
    // round-trips every field. Test names read as the spec.
    use super::*;
    fn gyro_packet() -> Vec<u8> {
        let mut buf = vec![0x10];
        buf.extend_from_slice(&1234u64.to_le_bytes());
        for value in [0.5f32, -0.2, 0.1, 1.0, 9.8, 0.0, 22.1, -45.3, 11.7] {
            buf.extend_from_slice(&value.to_le_bytes());
        }
        buf
    }
    fn hand_packet() -> Vec<u8> {
        let mut buf = vec![0x11];
        buf.extend_from_slice(&2345u64.to_le_bytes());
        buf.push(2);
        buf.push(21);
        buf.extend_from_slice(&0.91f32.to_le_bytes());
        buf
    }
    #[test]
    fn gyro_packet_length_matches_the_wire_contract() {
        assert_eq!(gyro_packet().len(), GYRO_PACKET_LEN);
    }
    #[test]
    fn parses_a_gyro_packet() {
        match parse_packet(&gyro_packet()) {
            TelemetryPacket::Gyro(GyroSample {
                timestamp_ms,
                angular_velocity,
                acceleration,
                magnetic_field,
            }) => {
                assert_eq!(timestamp_ms, 1234);
                assert_eq!(angular_velocity, [0.5, -0.2, 0.1]);
                assert_eq!(acceleration, [1.0, 9.8, 0.0]);
                assert_eq!(magnetic_field, [22.1, -45.3, 11.7]);
            }
            other => panic!("expected Gyro, got {other:?}"),
        }
    }
    #[test]
    fn parses_a_hand_packet() {
        match parse_packet(&hand_packet()) {
            TelemetryPacket::Hand(HandFrame {
                timestamp_ms,
                hands,
                landmarks_per_hand,
                confidence,
            }) => {
                assert_eq!(timestamp_ms, 2345);
                assert_eq!(hands, 2);
                assert_eq!(landmarks_per_hand, 21);
                assert_eq!(confidence, 0.91);
            }
            other => panic!("expected Hand, got {other:?}"),
        }
    }
    #[test]
    fn parses_ping_and_hello() {
        assert!(matches!(parse_packet(&[0x20]), TelemetryPacket::Ping));
        assert!(matches!(
            parse_packet(b"CARDBOARD_PHONE_HELLO v1"),
            TelemetryPacket::Hello(_)
        ));
    }
    #[test]
    fn hello_version_extracts_commit_count_suffix() {
        match parse_packet(b"CARDBOARD_PHONE_HELLO v1 542") {
            TelemetryPacket::Hello(v) => assert_eq!(v, "542"),
            other => panic!("expected Hello, got {other:?}"),
        }
        match parse_packet(b"CARDBOARD_PHONE_HELLO v1") {
            TelemetryPacket::Hello(v) => assert_eq!(v, ""),
            other => panic!("expected Hello, got {other:?}"),
        }
        assert!(phone_hello_version(b"random noise").is_none());
    }
    #[test]
    fn rejects_empty_garbage_and_truncated_binary_frames() {
        assert!(matches!(parse_packet(&[]), TelemetryPacket::Unknown));
        assert!(matches!(parse_packet(b"random noise"), TelemetryPacket::Unknown));
        assert!(matches!(parse_packet(&[0x10, 0, 0, 0]), TelemetryPacket::Unknown));
    }
    fn net_stats_packet() -> Vec<u8> {
        let mut buf = vec![0x13];
        buf.extend_from_slice(&5678u64.to_le_bytes());
        buf.extend_from_slice(&120u32.to_le_bytes());
        buf.extend_from_slice(&3u32.to_le_bytes());
        buf.extend_from_slice(&58.5f32.to_le_bytes());
        buf
    }
    #[test]
    fn net_stats_packet_length_matches_the_wire_contract() {
        assert_eq!(net_stats_packet().len(), NET_STATS_PACKET_LEN);
    }
    #[test]
    fn parses_a_net_stats_packet() {
        match parse_packet(&net_stats_packet()) {
            TelemetryPacket::NetStats(stats) => {
                assert_eq!(stats.timestamp_ms, 5678);
                assert_eq!(stats.frames_decoded, 120);
                assert_eq!(stats.stalls, 3);
                assert_eq!(stats.decoded_fps, 58.5);
            }
            other => panic!("expected NetStats, got {other:?}"),
        }
    }
    #[test]
    fn rejects_truncated_net_stats() {
        assert!(matches!(parse_packet(&[0x13, 0, 0, 0]), TelemetryPacket::Unknown));
    }
}