//! Wire format for the phone -> bridge telemetry link (UDP 42071).
//!
//! Binary packets are distinguished by their leading tag byte:
//!   * `0x10` sensor sample — u64 timestamp_ms, 3x f32 angular velocity,
//!     3x f32 acceleration, 3x f32 magnetic field (45 bytes total)
//!   * `0x11` hand frame — u64 timestamp_ms, u8 hands, u8 landmarks/hand,
//!     f32 confidence (15 bytes total)
//!   * `0x13` net stats — u64 timestamp_ms, u32 frames decoded since last
//!     report, u32 stall count (monotonic), f32 decoded fps (21 bytes total).
//!     Sent every ~2 s by the phone's NetStatsReporter; drives the bridge's
//!     adaptive bitrate.
//!   * `0x20` ping — a bare tag byte, used only to keep the link alive
//!
//! Plus one text frame: `CARDBOARD_PHONE_HELLO vN` when the phone first joins.
//! These bytes are part of the locked bridge contract — do not change them.

#[derive(Debug, Clone, Copy, Default)]
#[allow(dead_code)]
pub struct GyroSample {
    pub timestamp_ms: u64,
    pub angular_velocity: [f32; 3],
    pub acceleration: [f32; 3],
    pub magnetic_field: [f32; 3],
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(dead_code)]
pub struct HandFrame {
    pub timestamp_ms: u64,
    pub hands: u8,
    pub landmarks_per_hand: u8,
    pub confidence: f32,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct RotationSample {
    pub timestamp_ms: u64,
    pub quat: [f32; 4], // [w, x, y, z]
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(dead_code)]
pub struct NetStats {
    pub timestamp_ms: u64,
    pub frames_decoded: u32,
    pub stalls: u32,
    pub decoded_fps: f32,
}

#[derive(Debug, Clone, Copy)]
pub enum TelemetryPacket {
    Gyro(GyroSample),
    Hand(HandFrame),
    Rotation(RotationSample),
    NetStats(NetStats),
    Hello,
    Ping,
    Unknown,
}

/// Minimum length of a well-formed gyro packet (tag + 8 + 9*4 bytes = 45).
const GYRO_PACKET_LEN: usize = 45;
/// Minimum length of a well-formed hand packet (tag + 8 + 2 + 4 bytes).
const HAND_PACKET_LEN: usize = 15;
/// Minimum length of a rotation quaternion packet (tag + 8 + 4*4 bytes = 25).
const ROTATION_PACKET_LEN: usize = 25;
/// Minimum length of a net-stats packet (tag + 8 + 4 + 4 + 4 bytes = 21).
const NET_STATS_PACKET_LEN: usize = 21;

/// Parse one datagram into the coarsest packet type the bridge cares about.
/// Malformed or unrecognised data yields `Unknown` rather than an error, so
/// a stray packet can never kill the receive loop.
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
            if is_phone_hello(buf) {
                TelemetryPacket::Hello
            } else {
                TelemetryPacket::Unknown
            }
        }
    }
}

fn parse_gyro(buf: &[u8]) -> GyroSample {
    GyroSample {
        timestamp_ms: read_u64(&buf[1..9]),
        angular_velocity: [read_f32(buf, 9), read_f32(buf, 13), read_f32(buf, 17)],
        acceleration: [read_f32(buf, 21), read_f32(buf, 25), read_f32(buf, 29)],
        magnetic_field: [read_f32(buf, 33), read_f32(buf, 37), read_f32(buf, 41)],
    }
}

fn parse_hand(buf: &[u8]) -> HandFrame {
    HandFrame {
        timestamp_ms: read_u64(&buf[1..9]),
        hands: buf[9],
        landmarks_per_hand: buf[10],
        confidence: read_f32(buf, 11),
    }
}

fn parse_rotation(buf: &[u8]) -> RotationSample {
    RotationSample {
        timestamp_ms: read_u64(&buf[1..9]),
        quat: [read_f32(buf, 9), read_f32(buf, 13), read_f32(buf, 17), read_f32(buf, 21)],
    }
}

fn parse_net_stats(buf: &[u8]) -> NetStats {
    NetStats {
        timestamp_ms: read_u64(&buf[1..9]),
        frames_decoded: read_u32(buf, 9),
        stalls: read_u32(buf, 13),
        decoded_fps: read_f32(buf, 17),
    }
}

/// The greeting a phone sends on first contact over the telemetry link.
fn is_phone_hello(buf: &[u8]) -> bool {
    String::from_utf8_lossy(buf)
        .trim_start()
        .starts_with("CARDBOARD_PHONE_HELLO")
}

/// Read a little-endian u64 at `offset` (length guaranteed by `parse_packet`).
fn read_u64(buf: &[u8]) -> u64 {
    u64::from_le_bytes(buf.try_into().expect("fixed-size u64 slice"))
}

/// Read a little-endian f32 at `offset` (bounds guaranteed by `parse_packet`).
fn read_f32(buf: &[u8], offset: usize) -> f32 {
    f32::from_le_bytes(buf[offset..offset + 4].try_into().expect("fixed-size f32 slice"))
}

/// Read a little-endian u32 at `offset` (bounds guaranteed by `parse_packet`).
fn read_u32(buf: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(buf[offset..offset + 4].try_into().expect("fixed-size u32 slice"))
}

/// Encode the phone-side greeting (kept for tests/reference — the real phone
/// sends this over the wire).
#[allow(dead_code)]
pub fn encode_hello(version: u32) -> Vec<u8> {
    format!("CARDBOARD_PHONE_HELLO v{}", version).into_bytes()
}

#[allow(dead_code)]
pub fn encode_ack() -> Vec<u8> {
    b"BRIDGE_ACK".to_vec()
}

#[cfg(test)]
mod tests {
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
        buf.push(2); // hands
        buf.push(21); // landmarks per hand
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
            TelemetryPacket::Hello
        ));
    }

    #[test]
    fn rejects_empty_garbage_and_truncated_binary_frames() {
        assert!(matches!(parse_packet(&[]), TelemetryPacket::Unknown));
        assert!(matches!(parse_packet(b"random noise"), TelemetryPacket::Unknown));
        // 0x10 with too few payload bytes must not be misread as a gyro frame.
        assert!(matches!(parse_packet(&[0x10, 0, 0, 0]), TelemetryPacket::Unknown));
    }

    fn net_stats_packet() -> Vec<u8> {
        let mut buf = vec![0x13];
        buf.extend_from_slice(&5678u64.to_le_bytes());
        buf.extend_from_slice(&120u32.to_le_bytes()); // frames decoded
        buf.extend_from_slice(&3u32.to_le_bytes()); // stalls
        buf.extend_from_slice(&58.5f32.to_le_bytes()); // decoded fps
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