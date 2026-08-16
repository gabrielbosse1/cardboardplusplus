use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, RwLock};

// -- Protocol ports --
// 42071: Driver <-> Bridge (driver connects to bridge)
// 42072: Phone <-> Bridge (phone connects to bridge)
//
// Driver -> Bridge:  STATUS fps=X phone_connected=Y phone_fps=Z phone_fps_1low=W
//                    ACK
// Bridge -> Driver:  GET_STATUS / START_STREAM / STOP_STREAM
//                    SET_PHONE_FPS X / SET_PHONE_FPS1LOW X / SET_PHONE_CONNECTED true|false
//
// Phone -> Bridge:   REPORT_FPS fps=X 1low=Y
//                    CONNECT / DISCONNECT
// Bridge -> Phone:   SET_IPD X / GET_GYRO / PING

#[derive(Debug, Clone)]
pub struct BridgeState {
    // Driver connection state (populated from driver STATUS responses)
    pub driver_connected: bool,
    pub compositor_fps: f32,
    pub encoder_fps: f32,

    // Phone connection state (populated from phone TCP connection)
    pub phone_connected: bool,
    pub phone_fps: f32,
    pub phone_fps_1low: f32,
}

impl Default for BridgeState {
    fn default() -> Self {
        Self {
            driver_connected: false,
            compositor_fps: 0.0,
            encoder_fps: 0.0,
            phone_connected: false,
            phone_fps: 0.0,
            phone_fps_1low: 0.0,
        }
    }
}

pub struct ConnectionManager {
    state: Arc<RwLock<BridgeState>>,
    to_driver_tx: broadcast::Sender<String>,
    to_phone_tx: broadcast::Sender<String>,
    pub state_changed: broadcast::Sender<()>,
}

impl ConnectionManager {
    pub fn new() -> Arc<Self> {
        let (to_driver_tx, _) = broadcast::channel::<String>(32);
        let (to_phone_tx, _) = broadcast::channel::<String>(32);
        let (state_changed, _) = broadcast::channel(16);
        let state = Arc::new(RwLock::new(BridgeState::default()));

        let conn = Arc::new(Self {
            state: state.clone(),
            to_driver_tx: to_driver_tx.clone(),
            to_phone_tx: to_phone_tx.clone(),
            state_changed: state_changed.clone(),
        });

        // Spawn driver TCP server (port 42071)
        {
            let s = state.clone();
            let sc = state_changed.clone();
            let tdtx = to_driver_tx.clone();
            let tdtx_rx = to_driver_tx.subscribe();
            tokio::spawn(async move {
                Self::accept_loop("driver", "127.0.0.1:42071", s, sc, tdtx, tdtx_rx, Role::Driver).await;
            });
        }

        // Spawn phone TCP server (port 42072)
        {
            let s = state.clone();
            let sc = state_changed.clone();
            let tptx = to_phone_tx.clone();
            let tptx_rx = to_phone_tx.subscribe();
            tokio::spawn(async move {
                Self::accept_loop("phone", "0.0.0.0:42072", s, sc, tptx, tptx_rx, Role::Phone).await;
            });
        }

        conn
    }

    pub fn get_state(&self) -> Arc<RwLock<BridgeState>> {
        self.state.clone()
    }

    pub async fn send_command(&self, cmd: &str) {
        tracing::info!("[BRIDGE] Sending to driver: {}", cmd);
        let _ = self.to_driver_tx.send(format!("{}\n", cmd));
    }

    async fn accept_loop(
        name: &str,
        addr: &str,
        state: Arc<RwLock<BridgeState>>,
        state_changed: broadcast::Sender<()>,
        _to_target_tx: broadcast::Sender<String>,
        mut to_target_rx: broadcast::Receiver<String>,
        role: Role,
    ) {
        let name = name.to_string();
        let listener = match TcpListener::bind(addr).await {
            Ok(l) => l,
            Err(e) => {
                tracing::error!("[BRIDGE] TCP bind failed for {} on {}: {}", name, addr, e);
                return;
            }
        };
        tracing::info!("[BRIDGE] TCP server ready name={} addr={}", name, addr);

        loop {
            let (stream, peer) = match listener.accept().await {
                Ok(v) => v,
                Err(e) => {
                    tracing::error!("[BRIDGE] Accept failed name={} error={}", name, e);
                    continue;
                }
            };
            tracing::info!("[BRIDGE] Peer connected name={} peer={}", name, peer);

            let s = state.clone();
            let sc = state_changed.clone();
            let mut rx = to_target_rx.resubscribe();
            let peer_addr = peer.to_string();
            let name_clone = name.clone();

            tokio::spawn(async move {
                let result = match role {
                    Role::Driver => handle_driver(stream, s.clone(), sc.clone(), &mut rx).await,
                    Role::Phone => handle_phone(stream, s.clone(), sc.clone(), &mut rx).await,
                };
                match result {
                    Ok(()) => {
                        tracing::info!("[BRIDGE] Peer disconnected name={} peer={}", name_clone, peer_addr);
                    }
                    Err(e) => {
                        tracing::error!("[BRIDGE] Peer error name={} peer={} error={}", name_clone, peer_addr, e);
                    }
                }
                // Mark disconnected
                match role {
                    Role::Driver => {
                        let mut s = s.write().await;
                        s.driver_connected = false;
                        s.compositor_fps = 0.0;
                        s.encoder_fps = 0.0;
                        let _ = sc.send(());
                        tracing::info!("[BRIDGE] Driver state -> disconnected");
                    }
                    Role::Phone => {
                        let mut s = s.write().await;
                        s.phone_connected = false;
                        s.phone_fps = 0.0;
                        s.phone_fps_1low = 0.0;
                        let _ = sc.send(());
                        tracing::info!("[BRIDGE] Phone state -> disconnected");
                    }
                }
            });
        }
    }
}

enum Role {
    Driver,
    Phone,
}

impl Copy for Role {}
impl Clone for Role {
    fn clone(&self) -> Self { *self }
}

// -- Driver handler: bridge <-> SteamVR driver --
async fn handle_driver(
    stream: TcpStream,
    state: Arc<RwLock<BridgeState>>,
    state_changed: broadcast::Sender<()>,
    to_driver_rx: &mut broadcast::Receiver<String>,
) -> std::io::Result<()> {
    {
        let mut s = state.write().await;
        s.driver_connected = true;
        let _ = state_changed.send(());
        tracing::info!("[DRIVER] Connected, requesting status");
    }

    let (reader, mut writer) = stream.into_split();
    let mut lines = BufReader::new(reader).lines();

    // Request initial status
    writer.write_all(b"GET_STATUS\n").await?;

    let mut poll_interval = tokio::time::interval(std::time::Duration::from_secs(1));

    loop {
        tokio::select! {
            _ = poll_interval.tick() => {
                if writer.write_all(b"GET_STATUS\n").await.is_err() {
                    break;
                }
            }
            line = lines.next_line() => {
                match line {
                    Ok(Some(line)) => {
                        let line = line.trim().to_string();
                        if line.is_empty() { continue; }

                        tracing::debug!("[DRIVER] recv: {}", line);

                        if line.starts_with("STATUS ") {
                            parse_driver_status(&line, &state, &state_changed).await;
                        } else if line == "ACK" {
                            tracing::debug!("[DRIVER] ACK");
                        }
                    }
                    Ok(None) => {
                        tracing::info!("[DRIVER] Connection closed by peer");
                        break;
                    }
                    Err(e) => {
                        tracing::error!("[DRIVER] Read error: {}", e);
                        break;
                    }
                }
            }
            msg = to_driver_rx.recv() => {
                match msg {
                    Ok(msg) => {
                        tracing::debug!("[DRIVER] send: {}", msg.trim_end());
                        if writer.write_all(msg.as_bytes()).await.is_err() {
                            break;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        tracing::warn!("[DRIVER] Broadcast lagged by {} messages", n);
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }
        }
    }

    Ok(())
}

// -- Phone handler: bridge <-> Android app --
async fn handle_phone(
    stream: TcpStream,
    state: Arc<RwLock<BridgeState>>,
    state_changed: broadcast::Sender<()>,
    to_phone_rx: &mut broadcast::Receiver<String>,
) -> std::io::Result<()> {
    {
        let mut s = state.write().await;
        s.phone_connected = true;
        let _ = state_changed.send(());
        tracing::info!("[PHONE] Connected, waiting for reports");
    }

    let (reader, mut writer) = stream.into_split();
    let mut lines = BufReader::new(reader).lines();

    loop {
        tokio::select! {
            line = lines.next_line() => {
                match line {
                    Ok(Some(line)) => {
                        let line = line.trim().to_string();
                        if line.is_empty() { continue; }

                        tracing::debug!("[PHONE] recv: {}", line);

                        if line.starts_with("REPORT_FPS ") {
                            parse_phone_fps(&line, &state, &state_changed).await;
                        } else if line == "PING" {
                            tracing::debug!("[PHONE] PING -> PONG");
                            if writer.write_all(b"PONG\n").await.is_err() {
                                break;
                            }
                        }
                    }
                    Ok(None) => {
                        tracing::info!("[PHONE] Connection closed by peer");
                        break;
                    }
                    Err(e) => {
                        tracing::error!("[PHONE] Read error: {}", e);
                        break;
                    }
                }
            }
            msg = to_phone_rx.recv() => {
                match msg {
                    Ok(msg) => {
                        tracing::debug!("[PHONE] send: {}", msg.trim_end());
                        if writer.write_all(msg.as_bytes()).await.is_err() {
                            break;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        tracing::warn!("[PHONE] Broadcast lagged by {} messages", n);
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }
        }
    }

    Ok(())
}

// -- Protocol parsers --

async fn parse_driver_status(line: &str, state: &Arc<RwLock<BridgeState>>, state_changed: &broadcast::Sender<()>) {
    let mut compositor_fps = 0.0f32;
    let mut encoder_fps = 0.0f32;

    for part in line[7..].split_whitespace() {
        let mut kv = part.splitn(2, '=');
        if let (Some(key), Some(val)) = (kv.next(), kv.next()) {
            match key {
                "compositor_fps" => { if let Ok(v) = val.parse() { compositor_fps = v; } }
                "encoder_fps" => { if let Ok(v) = val.parse() { encoder_fps = v; } }
                _ => {}
            }
        }
    }

    let mut s = state.write().await;
    s.compositor_fps = compositor_fps;
    s.encoder_fps = encoder_fps;
    let _ = state_changed.send(());
    tracing::info!("[DRIVER] Status: compositor={:.1} encoder={:.1}", compositor_fps, encoder_fps);
}

async fn parse_phone_fps(line: &str, state: &Arc<RwLock<BridgeState>>, state_changed: &broadcast::Sender<()>) {
    let mut fps = 0.0f32;
    let mut fps_1low = 0.0f32;

    for part in line[11..].split_whitespace() {
        let mut kv = part.splitn(2, '=');
        if let (Some(key), Some(val)) = (kv.next(), kv.next()) {
            match key {
                "fps" => { if let Ok(v) = val.parse() { fps = v; } }
                "1low" => { if let Ok(v) = val.parse() { fps_1low = v; } }
                _ => {}
            }
        }
    }

    let mut s = state.write().await;
    s.phone_fps = fps;
    s.phone_fps_1low = fps_1low;
    let _ = state_changed.send(());
    tracing::info!("[PHONE] FPS report: fps={:.1} 1low={:.1}", fps, fps_1low);
}
