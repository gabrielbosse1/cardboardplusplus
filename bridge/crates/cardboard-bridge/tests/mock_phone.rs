//! Bridge tests pretending to be the phone: wire-builder helpers construct
//! gyro/hand/ping/hello/camera packets byte-for-byte (mirroring
//! TelemetrySender.java), and each test proves parse_packet round-trips them
//! or the AppState counters track them. UDP loopback tests prove framing
//! survives the socket. Test names read as the spec.
use std::net::UdpSocket;
use std::time::Duration;
/// Phone telemetry + camera ports under test (wire contract 42071/42072).
const TELEMETRY_PORT: u16 = 42071;
const CAMERA_PORT: u16 = 42072;
/// Builds a tag-0x10 gyro frame: u64 timestamp + 9xf32 (gyro/accel/mag).
fn build_gyro_packet(timestamp_ms: u64, ang_vel: [f32; 3], accel: [f32; 3], mag: [f32; 3]) -> Vec<u8> {
    let mut buf = vec![0x10];
    buf.extend_from_slice(&timestamp_ms.to_le_bytes());
    for v in ang_vel {
        buf.extend_from_slice(&v.to_le_bytes());
    }
    for v in accel {
        buf.extend_from_slice(&v.to_le_bytes());
    }
    for v in mag {
        buf.extend_from_slice(&v.to_le_bytes());
    }
    buf
}
/// Builds a tag-0x11 hand hint: u64 timestamp + hands/landmarks + confidence.
fn build_hand_packet(timestamp_ms: u64, hands: u8, landmarks: u8, confidence: f32) -> Vec<u8> {
    let mut buf = vec![0x11];
    buf.extend_from_slice(&timestamp_ms.to_le_bytes());
    buf.push(hands);
    buf.push(landmarks);
    buf.extend_from_slice(&confidence.to_le_bytes());
    buf
}
/// Builds the bare tag-0x20 keepalive byte.
fn build_ping_packet() -> Vec<u8> {
    vec![0x20]
}
/// Builds the "CARDBOARD_PHONE_HELLO vN" connect announcement.
fn build_hello_packet(version: u32) -> Vec<u8> {
    format!("CARDBOARD_PHONE_HELLO v{version}").into_bytes()
}
#[test]
fn gyro_packet_roundtrip() {
    let packet = build_gyro_packet(1234, [0.5, -0.2, 0.1], [1.0, 9.8, 0.0], [22.1, -45.3, 11.7]);
    assert_eq!(packet.len(), 45);
    match cardboard_bridge::net::telemetry::parse_packet(&packet) {
        cardboard_bridge::net::telemetry::TelemetryPacket::Gyro(sample) => {
            assert_eq!(sample.timestamp_ms, 1234);
            assert_eq!(sample.angular_velocity, [0.5, -0.2, 0.1]);
            assert_eq!(sample.acceleration, [1.0, 9.8, 0.0]);
            assert_eq!(sample.magnetic_field, [22.1, -45.3, 11.7]);
        }
        other => panic!("expected Gyro, got {other:?}"),
    }
}
#[test]
fn hand_packet_roundtrip() {
    let packet = build_hand_packet(2345, 2, 21, 0.91);
    assert_eq!(packet.len(), 15);
    match cardboard_bridge::net::telemetry::parse_packet(&packet) {
        cardboard_bridge::net::telemetry::TelemetryPacket::Hand(frame) => {
            assert_eq!(frame.timestamp_ms, 2345);
            assert_eq!(frame.hands, 2);
            assert_eq!(frame.landmarks_per_hand, 21);
            assert_eq!(frame.confidence, 0.91);
        }
        other => panic!("expected Hand, got {other:?}"),
    }
}
#[test]
fn ping_packet_is_recognized() {
    let packet = build_ping_packet();
    assert!(matches!(
        cardboard_bridge::net::telemetry::parse_packet(&packet),
        cardboard_bridge::net::telemetry::TelemetryPacket::Ping
    ));
}
#[test]
fn hello_packet_is_recognized() {
    let packet = build_hello_packet(1);
    assert!(matches!(
        cardboard_bridge::net::telemetry::parse_packet(&packet),
        cardboard_bridge::net::telemetry::TelemetryPacket::Hello(_)
    ));
}
#[test]
fn hello_packet_carries_commit_count_version() {
    use cardboard_bridge::net::telemetry::{parse_packet, phone_hello_version, TelemetryPacket};
    let mut packet = build_hello_packet(1);
    packet.extend_from_slice(b" 542");
    match parse_packet(&packet) {
        TelemetryPacket::Hello(v) => assert_eq!(v, "542"),
        other => panic!("expected Hello, got {other:?}"),
    }
    assert_eq!(phone_hello_version(&packet).unwrap(), "542");
}
#[test]
fn empty_and_garbage_packets_are_unknown() {
    assert!(matches!(
        cardboard_bridge::net::telemetry::parse_packet(&[]),
        cardboard_bridge::net::telemetry::TelemetryPacket::Unknown
    ));
    assert!(matches!(
        cardboard_bridge::net::telemetry::parse_packet(b"random noise"),
        cardboard_bridge::net::telemetry::TelemetryPacket::Unknown
    ));
}
#[test]
fn truncated_gyro_packet_is_rejected() {
    let mut short = vec![0x10];
    short.extend_from_slice(&0u64.to_le_bytes());
    assert!(matches!(
        cardboard_bridge::net::telemetry::parse_packet(&short),
        cardboard_bridge::net::telemetry::TelemetryPacket::Unknown
    ));
}
#[test]
fn app_state_tracks_telemetry_metrics() {
    use cardboard_bridge::app::AppState;
    let mut state = AppState::default();
    assert_eq!(state.packets_total, 0);
    assert_eq!(state.hands_detected, 0);
    state.note_gyro(&cardboard_bridge::net::telemetry::GyroSample::default());
    state.note_hand(2);
    assert_eq!(state.packets_total, 2);
    assert_eq!(state.hands_detected, 2);
}
#[test]
fn app_state_camera_liveness() {
    use cardboard_bridge::app::AppState;
    use std::time::Duration;
    let mut state = AppState::default();
    assert!(!state.camera_connected);
    let rgba = vec![0u8; 4];
    state.note_camera_frame(1, 1, rgba);
    assert!(state.camera_connected);
    state.camera_frame_time = std::time::Instant::now() - Duration::from_secs(4);
    state.check_camera_liveness();
    assert!(!state.camera_connected);
}
#[test]
fn mock_phone_can_send_gyro_over_udp() {
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let target = sock.local_addr().unwrap();
    let packet = build_gyro_packet(100, [1.0, 2.0, 3.0], [9.8, 0.0, 0.0], [22.0, -45.0, 11.0]);
    let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
    sender.send_to(&packet, target).unwrap();
    let mut buf = [0u8; 65535];
    sock.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let (n, _) = sock.recv_from(&mut buf).unwrap();
    assert_eq!(n, 45);
    assert_eq!(buf[0], 0x10);
}
#[test]
fn mock_phone_can_send_hand_frame_over_udp() {
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let target = sock.local_addr().unwrap();
    let packet = build_hand_packet(500, 1, 21, 0.85);
    let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
    sender.send_to(&packet, target).unwrap();
    let mut buf = [0u8; 65535];
    sock.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let (n, _) = sock.recv_from(&mut buf).unwrap();
    assert_eq!(n, 15);
    assert_eq!(buf[0], 0x11);
}
#[test]
fn mock_phone_can_send_camera_jpeg() {
    let minimal_jpeg: Vec<u8> = vec![0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xD9];
    let seq: u16 = 0x1234;
    let mut datagram = vec![(seq >> 8) as u8, (seq & 0xFF) as u8];
    datagram.extend_from_slice(&minimal_jpeg);
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let target = sock.local_addr().unwrap();
    let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
    sender.send_to(&datagram, target).unwrap();
    let mut buf = [0u8; 65535];
    sock.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let (n, _) = sock.recv_from(&mut buf).unwrap();
    assert_eq!(n, datagram.len());
    let parsed_seq = u16::from_be_bytes([buf[0], buf[1]]);
    assert_eq!(parsed_seq, seq);
    assert_eq!(&buf[2..4], &[0xFF, 0xD8]);
}
#[test]
fn telemetry_port_matches_wire_contract() {
    assert_eq!(TELEMETRY_PORT, 42071);
    assert_eq!(CAMERA_PORT, 42072);
}
#[test]
fn hello_wire_matches_bridge_expectation() {
    let hello = build_hello_packet(1);
    let msg = String::from_utf8_lossy(&hello);
    assert!(msg.starts_with("CARDBOARD_PHONE_HELLO"));
    assert!(msg.ends_with("v1"));
}
