//! Windows end-to-end suite: real bridge binary + fake driver/phone/sidecar,
//! verifying every control-plane path on the actual wire before the Linux
//! port. Fake-mode tests share one mutex (fixed UDP ports); run with
//! --test-threads=1 (scripts/e2e.ps1 does). CBPP_E2E_LIVE=1 switches to
//! driving a real bridge + SteamVR driver through REST only.
mod fakes;
mod harness;
use fakes::{sidecar, FakeDriver, FakeShm, Phone};
use harness::{
    artifacts, decode_frame_ppm, ffmpeg_available, live_mode, make_test_stream, serial,
    split_access_units, wait_until, working_set_mb, Bridge, LiveBridge,
};
use std::net::TcpStream;
use std::io::Write;
use std::time::Duration;
/// Every fake-mode test: the shared fake sidecar first (blocks the bridge
/// from spawning real Python on 42073), then a fresh bridge.
fn start_bridge() -> Bridge {
    let _sidecar = sidecar();
    Bridge::start()
}
#[test]
fn rest_plane_serves_health_status_and_index() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let (code, body) = bridge.http.get("/health");
    assert_eq!(code, 200);
    assert!(body.contains("\"ok\":true") && body.contains("app_version"));
    let status = bridge.status();
    for field in [
        "driver_connected",
        "phone_connected",
        "gyro_fps",
        "hands_detected",
        "preview_driver_fps",
        "camera_connected",
        "net_frames_decoded",
    ] {
        assert!(status.get(field).is_some(), "/status missing {field}: {status}");
    }
    assert_eq!(bridge.http.get("/").0, 200);
    assert_eq!(bridge.http.get("/nope").0, 404);
}
#[test]
fn phone_hello_connects_and_timeout_clears_it() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let phone = Phone::new();
    phone.hello("9.9.9-e2e");
    phone.gyro(1);
    bridge.wait_status("phone_connected", serde_json::json!(true), Duration::from_secs(5));
    assert_eq!(bridge.status_field("phone_ip"), serde_json::json!("127.0.0.1"));
    bridge.wait_log("phone version 9.9.9-e2e", Duration::from_secs(2));
    let gyro = bridge.status_field("latest_accel_y").as_f64().unwrap();
    assert!((gyro - 9.8).abs() < 0.01, "gyro values not folded into status: {gyro}");
    wait_until("phone_connected false after 4s silence", Duration::from_secs(9), || {
        !bridge.status_field("phone_connected").as_bool().unwrap_or(true)
    });
    bridge.wait_log("phone timed out", Duration::from_secs(1));
}
#[test]
fn driver_handshake_survives_mute_and_recovers() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let driver = FakeDriver::new();
    bridge.wait_status("driver_connected", serde_json::json!(true), Duration::from_secs(5));
    bridge.wait_log("driver handshake established", Duration::from_secs(1));
    driver.set_muted(true);
    wait_until("driver_connected false after ack timeout", Duration::from_secs(10), || {
        !bridge.status_field("driver_connected").as_bool().unwrap_or(true)
    });
    bridge.wait_log("driver heartbeat lost", Duration::from_secs(1));
    driver.set_muted(false);
    bridge.wait_status("driver_connected", serde_json::json!(true), Duration::from_secs(5));
}
#[test]
fn driver_stats_reach_status_and_preview() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let driver = FakeDriver::new();
    driver.set_stats(42, 9000, 777, 3);
    bridge.wait_status("preview_driver_fps", serde_json::json!(42), Duration::from_secs(5));
    assert_eq!(bridge.status_field("preview_bitrate_kbps"), serde_json::json!(9000));
    assert_eq!(bridge.status_field("preview_frames"), serde_json::json!(777));
    assert_eq!(bridge.status_field("preview_drops"), serde_json::json!(3));
    let preview = bridge.http.get_json("/preview");
    assert_eq!(preview["driver"]["fps"], serde_json::json!(42));
    assert_eq!(preview["driver"]["drops"], serde_json::json!(3));
}
#[test]
fn settings_post_lands_on_the_driver_wire() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let driver = FakeDriver::new();
    bridge.wait_status("driver_connected", serde_json::json!(true), Duration::from_secs(5));
    let (code, sent) = bridge.http.post_json(
        "/settings",
        r#"{"width":1920,"height":1080,"fps":50,"bitrate":12,"encoder":"h264_nvenc"}"#,
    );
    assert_eq!(code, 200);
    assert_eq!(sent["sent"]["width"], serde_json::json!(1920));
    driver.wait_capture("CARDBOARD_CAP 1920 1080", Duration::from_secs(3));
    driver.wait_capture("BRIDGE_CFG 50 12000 h264_nvenc", Duration::from_secs(3));
    bridge.wait_log("config applied: 1920x1080 @50fps 12mbps", Duration::from_secs(2));
    let (code, sent) = bridge.http.post_json("/settings", r#"{"fps":72}"#);
    assert_eq!(code, 200);
    assert_eq!(sent["sent"]["width"], serde_json::json!(1920), "partial POST must merge live width");
    driver.wait_capture("BRIDGE_CFG 72 12000 h264_nvenc", Duration::from_secs(3));
    assert_eq!(bridge.http.post("/settings", "not json").0, 400);
}
#[test]
fn camera_frames_drive_hand_detection_and_skeleton_forward() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let fake_mp = sidecar();
    fake_mp.set_hands(1);
    let bridge = start_bridge();
    let _driver = FakeDriver::new();
    let phone = Phone::new();
    phone.hello("1.0");
    bridge.wait_status("phone_connected", serde_json::json!(true), Duration::from_secs(5));
    let jpeg = fakes::test_jpeg();
    for seq in 0..30u16 {
        phone.camera(seq, &jpeg);
        std::thread::sleep(Duration::from_millis(60));
    }
    bridge.wait_status("camera_connected", serde_json::json!(true), Duration::from_secs(5));
    wait_until("camera_detected_hands >= 1", Duration::from_secs(10), || {
        bridge.status_field("camera_detected_hands").as_u64().unwrap_or(0) >= 1
    });
    fake_mp.set_hands(0);
    assert!(fake_mp.detects() > 0, "sidecar never received a detect request");
    let skel = _driver.wait_sensor(Duration::from_secs(10), |p| p.first() == Some(&0x13) && p.len() == 879);
    assert_eq!(skel.len(), 879, "skeleton packet must be tag+ts+hand+31 bones");
    bridge.wait_log("skeleton -> driver udp 42074", Duration::from_secs(2));
}
#[test]
fn phone_rotation_and_gyro_relay_verbatim_to_driver() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let driver = FakeDriver::new();
    let phone = Phone::new();
    phone.hello("1.0");
    bridge.wait_status("phone_connected", serde_json::json!(true), Duration::from_secs(5));
    let quat = [0.7071f32, 0.0, 0.7071, 0.0];
    phone.rotation(4242, quat);
    phone.gyro(4243);
    let rot = driver.wait_sensor(Duration::from_secs(5), |p| p.first() == Some(&0x12) && p.len() == 25);
    assert_eq!(&rot[1..9], &4242u64.to_le_bytes(), "rotation timestamp mangled in relay");
    assert_eq!(&rot[9..13], &quat[0].to_le_bytes(), "rotation quat mangled in relay");
    driver.wait_sensor(Duration::from_secs(5), |p| p.first() == Some(&0x10) && p.len() == 45);
}
#[test]
fn stream_frames_captured_every_half_second() {
    let _gate = serial();
    if live_mode() || !ffmpeg_available() {
        eprintln!("skipping: live mode or no ffmpeg on PATH");
        return;
    }
    let bridge = start_bridge();
    let driver = FakeDriver::new();
    let phone = Phone::new();
    phone.hello("1.0");
    driver.set_phone_port(phone.local_port());
    phone.beacon();
    assert_eq!(phone.recv_text(Duration::from_secs(3)).as_deref(), Some("ACK"));
    let cap = phone.recv_text(Duration::from_secs(3));
    assert_eq!(cap.as_deref(), Some("CARDBOARD_CAP 1920 1080"), "driver must echo the applied size");
    let h264 = artifacts().join("source.h264");
    assert!(make_test_stream(&h264), "ffmpeg failed to encode the test stream");
    let aus = split_access_units(&std::fs::read(&h264).unwrap());
    assert!(aus.len() >= 90, "expected 3s @30fps of access units, got {}", aus.len());
    let mut raw: Vec<u8> = Vec::new();
    let mut recv_buf = [0u8; 2048];
    let started = std::time::Instant::now();
    let total = aus.len().min(100);
    std::thread::scope(|s| {
        s.spawn(|| driver.stream_h264(&aus[..total]));
        loop {
            match phone.recv_into(&mut recv_buf, Duration::from_millis(400)) {
                Some(n) => raw.extend_from_slice(&recv_buf[..n]),
                None => break,
            }
            if started.elapsed() > Duration::from_secs(15) {
                break;
            }
        }
    });
    assert!(!raw.is_empty(), "phone socket received no video bytes");
    let got = split_length_prefixed(&raw);
    assert!(got.len() >= 60, "phone got {} of {total} length-prefixed AUs", got.len());
    // The stream runs at 30fps, so every 15th access unit is one captured
    // frame per 500ms of stream time: 0ms, 500ms, 1000ms.
    let dir = artifacts();
    for (slot, idx) in [0usize, 15, 30].into_iter().enumerate() {
        let au = &got[idx];
        let name = format!("frame_{}", slot + 1);
        std::fs::write(dir.join(format!("{name}.h264")), au).unwrap();
        assert!(decode_frame_ppm(au, &dir, &name), "captured frame {name} (stream AU {idx}) does not decode");
    }
    assert!(got.iter().any(|au| contains_nal(au, 7)), "no SPS NAL found in the captured stream");
    let _ = bridge;
}
/// True when the Annex-B access unit contains a NAL of `nal_type`.
fn contains_nal(au: &[u8], nal_type: u8) -> bool {
    let mut i = 0;
    while i + 3 < au.len() {
        if au[i] == 0 && au[i + 1] == 0 {
            if au[i + 2] == 1 && (au[i + 3] & 0x1f) == nal_type {
                return true;
            }
            if au[i + 2] == 0 && i + 4 < au.len() && au[i + 3] == 1 && (au[i + 4] & 0x1f) == nal_type {
                return true;
            }
        }
        i += 1;
    }
    false
}
/// Reassembles the driver->phone framing: u32 BE length + AU, chunked to
/// arbitrary datagram boundaries.
fn split_length_prefixed(raw: &[u8]) -> Vec<Vec<u8>> {
    let mut aus = Vec::new();
    let mut i = 0;
    while i + 4 <= raw.len() {
        let len = u32::from_be_bytes(raw[i..i + 4].try_into().unwrap()) as usize;
        if len == 0 || i + 4 + len > raw.len() {
            break;
        }
        aus.push(raw[i + 4..i + 4 + len].to_vec());
        i += 4 + len;
    }
    aus
}
#[test]
fn shm_region_attaches_and_drains_telemetry() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let shm = FakeShm::create();
    bridge.wait_log("shm: attached to driver status region", Duration::from_secs(8));
    shm.publish_telemetry(111);
    shm.publish_telemetry(222);
    bridge.wait_log("[shm] drained", Duration::from_secs(3));
}
#[test]
fn log_ring_caps_at_max_lines() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    bridge.http.get_json("/status");
    let logs = bridge.http.get_json("/logs?n=100000");
    let count = logs["logs"].as_array().unwrap().len();
    assert!(count <= 200 && count > 0, "ring log returned {count} lines, cap is 200");
}
#[test]
fn debug_flag_gates_verbose_logging_at_runtime() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    assert_eq!(bridge.http.get_json("/debug")["debug"], serde_json::json!(true), "CARDBOARD_DEBUG=1 must start verbose");
    let phone = Phone::new();
    phone.garbage(7);
    bridge.wait_log("ignored unknown 7-byte datagram", Duration::from_secs(3));
    let (code, body) = bridge.http.post_json("/debug", r#"{"enabled":false}"#);
    assert_eq!(code, 200);
    assert_eq!(body["debug"], serde_json::json!(false));
    std::thread::sleep(Duration::from_millis(300));
    let before = bridge.logs(200).iter().filter(|l| l.starts_with("REST ")).count();
    std::thread::sleep(Duration::from_millis(300));
    let after = bridge.logs(200).iter().filter(|l| l.starts_with("REST ")).count();
    assert_eq!(after, before, "REST access lines still logging with debug off");
    bridge.http.post("/debug", r#"{"enabled":true}"#);
    assert_eq!(bridge.http.get_json("/debug")["debug"], serde_json::json!(true));
    assert_eq!(bridge.http.post("/debug", "nope").0, 400);
}
#[test]
fn garbage_and_tiny_packets_never_kill_the_bridge() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    let phone = Phone::new();
    phone.garbage(1);
    phone.garbage(65500);
    let sock = std::net::UdpSocket::bind("0.0.0.0:0").unwrap();
    sock.send_to(b"", "127.0.0.1:42071").ok();
    sock.send_to(b"\x10short", "127.0.0.1:42071").ok();
    sock.send_to(&[0u8; 3][..], "127.0.0.1:42072").ok();
    let mut tcp = TcpStream::connect("127.0.0.1:42073").unwrap();
    tcp.write_all(b"\xff\xff\xff\xffgarbage").ok();
    drop(tcp);
    for _ in 0..5 {
        let (code, _) = bridge.http.get("/health");
        assert_eq!(code, 200, "bridge died on garbage input");
        std::thread::sleep(Duration::from_millis(200));
    }
}
#[test]
fn memory_stays_flat_under_sustained_telemetry() {
    let _gate = serial();
    if live_mode() {
        return;
    }
    let bridge = start_bridge();
    std::thread::sleep(Duration::from_secs(2));
    let base = working_set_mb(bridge.pid()).expect("working set of the bridge pid");
    let phone = Phone::new();
    let jpeg = fakes::test_jpeg();
    let started = std::time::Instant::now();
    for i in 0..5000u64 {
        phone.gyro(i);
        if i % 50 == 0 {
            phone.rotation(i, [1.0, 0.0, 0.0, 0.0]);
            phone.net_stats(i, i as u32, 0, 60.0);
        }
        if i % 500 == 0 {
            phone.camera((i / 500) as u16, &jpeg);
        }
    }
    let _elapsed = started.elapsed();
    std::thread::sleep(Duration::from_secs(3));
    let after = working_set_mb(bridge.pid()).expect("working set after soak");
    assert!(after < base + 96, "bridge working set grew {} MB -> {} MB under 5k telemetry packets", after - base, after);
    let logs = bridge.http.get_json("/logs?n=100000");
    assert!(logs["logs"].as_array().unwrap().len() <= 200, "ring log must stay capped through soak");
    assert_eq!(bridge.http.get("/health").0, 200);
}
#[test]
fn live_bridge_status_and_wire_with_real_driver() {
    if !live_mode() {
        eprintln!("set CBPP_E2E_LIVE=1 with a running bridge + SteamVR driver to run this");
        return;
    }
    let live = LiveBridge::connect();
    let status = live.0.get_json("/status");
    assert_eq!(status["driver_connected"], serde_json::json!(true), "SteamVR driver must be connected for live mode");
    let fps = status["preview_driver_fps"].as_i64().unwrap_or(0);
    assert!(fps > 0, "driver reports {fps} fps â€” stream a session in SteamVR first");
    let (code, _) = live.0.post_json("/settings", r#"{"fps":42}"#);
    assert_eq!(code, 200);
    let http = live.0;
    let result = (|| {
        for _ in 0..60 {
            let s = http.get_json("/status");
            if s["preview_driver_fps"].as_i64() == Some(42) {
                return true;
            }
            std::thread::sleep(Duration::from_millis(500));
        }
        false
    })();
    http.post_json("/settings", r#"{"fps":60}"#);
    assert!(result, "driver never confirmed fps=42 through BRIDGE_STATS");
}
