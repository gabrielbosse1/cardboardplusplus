//! Wire format for the phone -> bridge telemetry link (UDP 42071).
//!
//! Binary packets are distinguished by their leading tag byte:
//!   * `0x10` gyro sample — u64 timestamp_ms, 3x f32 angular velocity, 3x f32
//!     acceleration (33 bytes total)
//!   * `0x11` hand frame — u64 timestamp_ms, u8 hands, u8 landmarks/hand,
//!     f32 confidence (15 bytes total)
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
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(dead_code)]
pub struct HandFrame {
    pub timestamp_ms: u64,
    pub hands: u8,
    pub landmarks_per_hand: u8,
    pub confidence: f32,
}

#[derive(Debug, Clone, Copy)]
pub enum TelemetryPacket {
    Gyro(GyroSample),
    Hand(HandFrame),
    Hello,
    Ping,
    Unknown,
}

/// Minimum length of a well-formed gyro packet (tag + 8 + 6*4 bytes).
const GYRO_PACKET_LEN: usize = 33;
/// Minimum length of a well-formed hand packet (tag + 8 + 2 + 4 bytes).
const HAND_PACKET_LEN: usize = 15;

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
        for value in [0.5f32, -0.2, 0.1, 1.0, 9.8, 0.0] {
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
            }) => {
                assert_eq!(timestamp_ms, 1234);
                assert_eq!(angular_velocity, [0.5, -0.2, 0.1]);
                assert_eq!(acceleration, [1.0, 9.8, 0.0]);
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
}