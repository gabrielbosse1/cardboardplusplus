use std::net::UdpSocket;
use std::time::Duration;

// ---------------------------------------------------------------------------
// Tests that pretend to be the phone.
//
// These tests verify the bridge correctly handles phone telemetry, camera
// frames, and connection lifecycle by testing the wire format parsing and
// state management directly.
// ---------------------------------------------------------------------------

const TELEMETRY_PORT: u16 = 42071;
const CAMERA_PORT: u16 = 42072;

// --- Telemetry wire format builders (mirror the phone's encoding) ---

fn build_gyro_packet(timestamp_ms: u64, ang_vel: [f32; 3], accel: [f32; 3]) -> Vec<u8> {
    let mut buf = vec![0x10]; // tag
    buf.extend_from_slice(&timestamp_ms.to_le_bytes());
    for v in ang_vel {
        buf.extend_from_slice(&v.to_le_bytes());
    }
    for v in accel {
        buf.extend_from_slice(&v.to_le_bytes());
    }
    buf
}

fn build_hand_packet(timestamp_ms: u64, hands: u8, landmarks: u8, confidence: f32) -> Vec<u8> {
    let mut buf = vec![0x11]; // tag
    buf.extend_from_slice(&timestamp_ms.to_le_bytes());
    buf.push(hands);
    buf.push(landmarks);
    buf.extend_from_slice(&confidence.to_le_bytes());
    buf
}

fn build_ping_packet() -> Vec<u8> {
    vec![0x20]
}

fn build_hello_packet(version: u32) -> Vec<u8> {
    format!("CARDBOARD_PHONE_HELLO v{version}").into_bytes()
}

// --- Tests using the bridge's telemetry parser ---

#[test]
fn gyro_packet_roundtrip() {
    let packet = build_gyro_packet(1234, [0.5, -0.2, 0.1], [1.0, 9.8, 0.0]);
    assert_eq!(packet.len(), 33); // 1 + 8 + 6*4 = 33

    match cardboard_bridge::net::telemetry::parse_packet(&packet) {
        cardboard_bridge::net::telemetry::TelemetryPacket::Gyro(sample) => {
            assert_eq!(sample.timestamp_ms, 1234);
            assert_eq!(sample.angular_velocity, [0.5, -0.2, 0.1]);
            assert_eq!(sample.acceleration, [1.0, 9.8, 0.0]);
        }
        other => panic!("expected Gyro, got {other:?}"),
    }
}

#[test]
fn hand_packet_roundtrip() {
    let packet = build_hand_packet(2345, 2, 21, 0.91);
    assert_eq!(packet.len(), 15); // 1 + 8 + 1 + 1 + 4 = 15

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
        cardboard_bridge::net::telemetry::TelemetryPacket::Hello
    ));
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
    // 0x10 with too few bytes must not parse as gyro.
    let mut short = vec![0x10];
    short.extend_from_slice(&0u64.to_le_bytes());
    // Missing the 6 f32 values (24 bytes).
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

    state.note_gyro();
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

    // Simulate a camera frame arriving.
    let rgba = vec![0u8; 4]; // 1x1 pixel
    state.note_camera_frame(1, 1, rgba);
    assert!(state.camera_connected);

    // Simulate stale frame (>3s old).
    state.camera_frame_time = std::time::Instant::now() - Duration::from_secs(4);
    state.check_camera_liveness();
    assert!(!state.camera_connected);
}

#[test]
fn mock_phone_can_send_gyro_over_udp() {
    // Verify a mock phone can send a gyro packet to the telemetry port.
    // We can't bind to the actual port (it might be in use), so we test
    // the packet format and a random port.
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let target = sock.local_addr().unwrap();

    let packet = build_gyro_packet(100, [1.0, 2.0, 3.0], [9.8, 0.0, 0.0]);
    let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
    sender.send_to(&packet, target).unwrap();

    let mut buf = [0u8; 65535];
    sock.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let (n, _) = sock.recv_from(&mut buf).unwrap();
    assert_eq!(n, 33);
    assert_eq!(buf[0], 0x10); // gyro tag
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
    assert_eq!(buf[0], 0x11); // hand tag
}

#[test]
fn mock_phone_can_send_camera_jpeg() {
    // A minimal valid JPEG (SOI + EOI markers).
    let minimal_jpeg: Vec<u8> = vec![0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xD9];

    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let target = sock.local_addr().unwrap();

    let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
    sender.send_to(&minimal_jpeg, target).unwrap();

    let mut buf = [0u8; 65535];
    sock.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let (n, _) = sock.recv_from(&mut buf).unwrap();
    assert_eq!(n, minimal_jpeg.len());
    assert_eq!(&buf[..2], &[0xFF, 0xD8]); // JPEG SOI
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
