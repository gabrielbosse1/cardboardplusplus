//! Live sidecar tests (#[ignore] by default): spawn the real Python
//! mediapipe_server.py + model and prove detect/config round-trips and the
//! camera pipeline's lazy-connect. Skipped when python/model is missing;
//! serialized by SIDECAR_LOCK since both bind TCP 42073. Run with --ignored.
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};
/// MediaPipe sidecar port (wire contract 42073).
const PORT: u16 = 42073;
/// Serializes the two live tests: each spawns its own sidecar on PORT.
static SIDECAR_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
/// Garbage JPEG (valid SOI, corrupt body): the model must answer zero hands,
/// never an error.
fn undecodable_jpeg() -> Vec<u8> {
    vec![0xFF, 0xD8, 0xFF, 0x00, 0x11, 0x22, 0x33, 0x44]
}
/// Locates mediapipe_server.py next to this crate. None when testing an
/// installed layout without the script (callers SKIP).
fn script_path() -> Option<PathBuf> {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("mediapipe_server.py");
    p.is_file().then_some(p)
}
/// True when the hand-landmarker model file is vendored. Without it the
/// sidecar cannot start and both tests SKIP.
fn model_present() -> bool {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("models")
        .join("hand_landmarker.task")
        .is_file()
}
/// Spawns the real sidecar (PYTHON env or PATH python) with output silenced.
/// Returns None when the script/model/python is missing or spawn fails, in
/// which case callers SKIP the test.
fn spawn_server() -> Option<Child> {
    let script = script_path()?;
    if !model_present() {
        return None;
    }
    let python = std::env::var("PYTHON").unwrap_or_else(|_| "python".into());
    Command::new(python)
        .arg(script)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .ok()
}
/// Waits up to `timeout` for the sidecar to accept TCP and ack a config
/// probe frame. False when the child dies first or the deadline passes.
fn wait_for_server(child: &mut Child, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if let Ok(mut s) = TcpStream::connect(format!("127.0.0.1:{PORT}")) {
            let _ = s.set_read_timeout(Some(Duration::from_secs(5)));
            let _ = s.set_write_timeout(Some(Duration::from_secs(5)));
            let mut buf = vec![0xFFu8, 0xFF, 0xFF, 0xFF, 0x01];
            for v in [0.5f32, 0.5, 0.5] {
                buf.extend_from_slice(&v.to_le_bytes());
            }
            if s.write_all(&buf).is_err() {
                return false;
            }
            let mut ack = [0u8; 1];
            if s.read_exact(&mut ack).is_ok() {
                return true;
            }
            return false;
        }
        if child.try_wait().ok().flatten().is_some() {
            return false;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    false
}
#[test]
#[ignore]
fn real_sidecar_detect_and_config_roundtrip() {
    let _guard = SIDECAR_LOCK.lock().unwrap();
    let Some(mut child) = spawn_server() else {
        eprintln!("SKIP: mediapipe_server.py, model file, or python missing");
        return;
    };
    let alive = wait_for_server(&mut child, Duration::from_secs(60));
    assert!(alive, "sidecar never answered on 127.0.0.1:{PORT} (see server stderr)");
    let client = cardboard_bridge::net::mediapipe::MediapipeClient::connect(PORT)
        .expect("rust client connects to real sidecar");
    let hands = client.detect(&undecodable_jpeg());
    assert!(hands.is_empty(), "garbage frame must yield zero hands");
    assert!(
        client.set_config(0.8, 0.7, 0.6),
        "set_config must be acked by real sidecar"
    );
    let hands = client.detect(&undecodable_jpeg());
    assert!(hands.is_empty(), "detect still works after set_config");
    let _ = child.kill();
    let _ = child.wait();
}
#[test]
#[ignore]
fn camera_pipeline_lazy_connects_without_startup_client() {
    use std::net::UdpSocket;
    use std::sync::{Arc, Mutex};
    let _guard = SIDECAR_LOCK.lock().unwrap();
    let jpg_out = std::env::temp_dir().join("hand_sidecar_test.jpg");
    let gen = Command::new(std::env::var("PYTHON").unwrap_or_else(|_| "python".into()))
        .arg("-c")
        .arg("import cv2,numpy as np,sys;img=np.full((192,256,3),128,dtype=np.uint8);_,b=cv2.imencode('.jpg',img);open(sys.argv[1],'wb').write(bytes(b))")
        .arg(&jpg_out)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    assert!(gen.map(|s| s.success()).unwrap_or(false), "need cv2 to build test frame");
    let jpeg = std::fs::read(&jpg_out).expect("test frame written");
    let Some(mut child) = spawn_server() else {
        eprintln!("SKIP: mediapipe_server.py, model file, or python missing");
        return;
    };
    assert!(
        wait_for_server(&mut child, Duration::from_secs(60)),
        "sidecar never answered on 127.0.0.1:{PORT}"
    );
    let state: cardboard_bridge::app::SharedState =
        Arc::new(Mutex::new(cardboard_bridge::app::AppState::default()));
    {
        let mut s = state.lock().unwrap();
        s.hand_enabled = true;
    }
    cardboard_bridge::net::camera::spawn(state.clone(), None);
    let sock = UdpSocket::bind("0.0.0.0:0").expect("test udp socket");
    for seq in 0u16..5 {
        let mut dg = seq.to_be_bytes().to_vec();
        dg.extend_from_slice(&jpeg);
        sock.send_to(&dg, "127.0.0.1:42072").expect("send frame");
        std::thread::sleep(Duration::from_millis(200));
    }
    let deadline = Instant::now() + Duration::from_secs(20);
    let mut connected = false;
    let mut lazy = false;
    while Instant::now() < deadline {
        if let Ok(s) = state.lock() {
            connected = s.camera_connected;
            lazy = s.log.iter().any(|l| l.contains("lazy-connected"));
            if connected && lazy {
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    assert!(connected, "camera frames must reach state (display path)");
    assert!(lazy, "detect worker must lazy-connect to the sidecar (no startup client)");
    assert_eq!(
        state.lock().unwrap().camera_detected_hands, 0,
        "blank frame must detect zero hands"
    );
    let _ = child.kill();
    let _ = child.wait();
}
