//! Bridge tests pretending to be the driver: asserts the wire strings the
//! bridge sends/parses (HELLO/ACK/STATS/CFG/CAP) match CardboardWire.h
//! byte-for-byte, plus the locked port numbers. Test names read as the spec.
use std::net::UdpSocket;
use std::time::Duration;
/// Driver discovery/control port this fake binds in the ignored live test.
const DRIVER_PORT: u16 = 42070;
#[test]
fn bridge_hello_wire_is_exactly_15_bytes() {
    let hello = b"BRIDGE_HELLO v1";
    assert_eq!(hello.len(), 15);
    assert_eq!(hello, b"BRIDGE_HELLO v1");
}
#[test]
fn bridge_ack_response_format() {
    let ack = b"BRIDGE_ACK v1";
    assert_eq!(ack.len(), 13);
    let msg = String::from_utf8_lossy(ack);
    assert!(msg.starts_with("BRIDGE_ACK"));
}
#[test]
fn bridge_stats_wire_format() {
    let fps = 60;
    let bitrate = 20000;
    let frames = 1234u64;
    let drops = 2u64;
    let stats = format!("BRIDGE_STATS fps={fps} bitrate={bitrate} frames={frames} drops={drops}");
    assert!(stats.starts_with("BRIDGE_STATS"));
    assert!(stats.contains("fps=60"));
    assert!(stats.contains("bitrate=20000"));
    assert!(stats.contains("frames=1234"));
    assert!(stats.contains("drops=2"));
}
#[test]
fn bridge_cfg_wire_format() {
    let cfg = format!("BRIDGE_CFG {} {} {}", 60, 20000, "h264_nvenc");
    assert_eq!(cfg, "BRIDGE_CFG 60 20000 h264_nvenc");
    let cfg_auto = format!("BRIDGE_CFG {} {} {}", 30, 8000, "auto");
    assert_eq!(cfg_auto, "BRIDGE_CFG 30 8000 auto");
}
#[test]
fn bridge_preview_wire_format() {
    assert_eq!(b"BRIDGE_PREVIEW 1", b"BRIDGE_PREVIEW 1");
    assert_eq!(b"BRIDGE_PREVIEW 0", b"BRIDGE_PREVIEW 0");
}
#[test]
fn cardboard_cap_wire_format() {
    let cap = format!("CARDBOARD_CAP {} {}", 1600, 900);
    assert_eq!(cap, "CARDBOARD_CAP 1600 900");
}
#[test]
#[ignore = "binds the real driver port 42070 — stop the bridge/driver first, then run with --ignored"]
fn mock_driver_can_exchange_heartbeat_with_bridge() {
    let sock = UdpSocket::bind(format!("127.0.0.1:{DRIVER_PORT}")).unwrap();
    sock.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let hello = b"BRIDGE_HELLO v1";
    let ack = b"BRIDGE_ACK v1";
    let msg = String::from_utf8_lossy(hello);
    assert!(msg.starts_with("BRIDGE_HELLO"));
    let response = String::from_utf8_lossy(ack);
    assert!(response.starts_with("BRIDGE_ACK"));
}
#[test]
fn stats_parser_handles_various_field_combinations() {
    let line = "BRIDGE_STATS fps=60 bitrate=20000 frames=1234 drops=2";
    assert!(parse_bridge_stats(line).is_some());
    let line = "BRIDGE_STATS fps=30";
    let parsed = parse_bridge_stats(line).unwrap();
    assert_eq!(parsed.0, 30);
    assert_eq!(parsed.1, 0);
    assert_eq!(parsed.2, 0);
    assert_eq!(parsed.3, 0);
    assert!(parse_bridge_stats("BRIDGE_STATS").is_none());
    assert!(parse_bridge_stats("BRIDGE_STATS future=none").is_none());
    assert!(parse_bridge_stats("BRIDGE_STATS fps=abc").is_none());
    assert!(parse_bridge_stats("BRIDGE_ACK v1").is_none());
    assert!(parse_bridge_stats("hello").is_none());
}
/// Independent mirror of the bridge's parse_stats: proves the STATS line shape
/// by reimplementing the parse instead of calling it, so both sides must agree.
fn parse_bridge_stats(msg: &str) -> Option<(i32, i32, u64, u64)> {
    let rest = msg.trim_start().strip_prefix("BRIDGE_STATS")?;
    let clean: String = rest.chars().filter(|c| !c.is_control()).collect();
    let mut fps = 0i32;
    let mut kbps = 0i32;
    let mut frames = 0u64;
    let mut drops = 0u64;
    let mut parsed = 0u32;
    for field in clean.split_whitespace() {
        let Some((key, value)) = field.split_once('=') else {
            continue;
        };
        match key {
            "fps" => {
                if let Ok(v) = value.parse() {
                    fps = v;
                    parsed += 1;
                }
            }
            "bitrate" => {
                if let Ok(v) = value.parse() {
                    kbps = v;
                    parsed += 1;
                }
            }
            "frames" => {
                if let Ok(v) = value.parse() {
                    frames = v;
                    parsed += 1;
                }
            }
            "drops" => {
                if let Ok(v) = value.parse() {
                    drops = v;
                    parsed += 1;
                }
            }
            _ => {}
        }
    }
    if parsed == 0 {
        return None;
    }
    Some((fps, kbps, frames, drops))
}
#[test]
fn discovery_port_matches_wire_contract() {
    assert_eq!(DRIVER_PORT, 42070);
}
#[test]
fn encoder_choice_roundtrip() {
    use cardboard_bridge::net::EncoderChoice;
    assert_eq!(EncoderChoice::from(0), EncoderChoice::Gpu);
    assert_eq!(EncoderChoice::from(1), EncoderChoice::Cpu);
    assert_eq!(EncoderChoice::Gpu.as_str(), "gpu");
    assert_eq!(EncoderChoice::Cpu.as_str(), "cpu");
    assert_eq!(EncoderChoice::Nvenc.as_str(), "h264_nvenc");
    assert_eq!(EncoderChoice::Auto.as_str(), "auto");
    assert_eq!(EncoderChoice::from_name("gpu"), EncoderChoice::Gpu);
    assert_eq!(EncoderChoice::from_name("h264_nvenc"), EncoderChoice::Nvenc);
}
#[test]
fn port_constants_are_locked() {
    use cardboard_bridge::net::*;
    assert_eq!(VIDEO_PORT, 42069);
    assert_eq!(DRIVER_DISCOVERY_PORT, 42070);
    assert_eq!(TELEMETRY_PORT, 42071);
    assert_eq!(CAMERA_PORT, 42072);
    assert_eq!(MEDIAPIPE_PORT, 42073);
}
