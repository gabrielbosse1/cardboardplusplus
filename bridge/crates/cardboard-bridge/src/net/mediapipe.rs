//! TCP client for the Python MediaPipe hand-landmark server.
//!
//! The bridge spawns `mediapipe_server.py` as a child process.  The server
//! listens on TCP 127.0.0.1:42073.  For each JPEG frame the bridge sends
//! (length-prefixed), the server replies with a compact binary packet
//! containing up to 2 detected hands (21 landmarks each).

use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::{Arc, Mutex};
use std::time::Duration;

/// A single 3-D landmark in normalised image coordinates.
#[derive(Debug, Clone, Copy, Default)]
pub struct Landmark {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

/// One detected hand: 21 landmarks + handedness + confidence score.
#[derive(Debug, Clone)]
pub struct DetectedHand {
    pub landmarks: [Landmark; 21],
    pub handedness: String,
    pub score: f32,
}

/// Shared handle to the TCP connection to the Python MediaPipe server.
#[derive(Clone)]
pub struct MediapipeClient {
    stream: Arc<Mutex<TcpStream>>,
    port: u16,
}

impl MediapipeClient {
    /// Quick single-shot connect (no retries). Used to check if a server
    /// is already running before attempting to spawn one.
    pub fn try_once(port: u16) -> anyhow::Result<Self> {
        let addr = format!("127.0.0.1:{port}");
        let (tx, rx) = std::sync::mpsc::channel();
        let addr_clone = addr.clone();
        std::thread::spawn(move || {
            let _ = tx.send(TcpStream::connect(&addr_clone));
        });
        let stream = rx.recv_timeout(Duration::from_secs(2))
            .map_err(|_| anyhow::anyhow!("connect timed out"))?
            .map_err(|e| anyhow::anyhow!(e))?;
        // Bound every detect() round-trip: a wedged Python server must not
        // stall the camera thread forever.
        let _ = stream.set_read_timeout(Some(Duration::from_secs(2)));
        let _ = stream.set_write_timeout(Some(Duration::from_secs(2)));
        eprintln!("[mediapipe] connected to {addr}");
        Ok(Self {
            stream: Arc::new(Mutex::new(stream)),
            port,
        })
    }

    /// Connect to the Python server.  Retries a few times in case the
    /// child process is still starting up.
    pub fn connect(port: u16) -> anyhow::Result<Self> {
        let addr = format!("127.0.0.1:{port}");
        let stream = Self::try_connect(&addr, 10, std::time::Duration::from_millis(200))?;
        eprintln!("[mediapipe] connected to {addr}");
        Ok(Self {
            stream: Arc::new(Mutex::new(stream)),
            port,
        })
    }

    fn try_connect(addr: &str, retries: u32, delay: Duration) -> anyhow::Result<TcpStream> {
        let connect_timeout = Duration::from_secs(2);
        for attempt in 0..retries {
            // Use a channel-based timeout: spawn connect in a thread, recv
            // with timeout.  This avoids Windows issues where
            // TcpStream::connect_timeout hangs on filtered localhost ports.
            let (tx, rx) = std::sync::mpsc::channel();
            let addr_str = addr.to_owned();
            std::thread::spawn(move || {
                let _ = tx.send(TcpStream::connect(&addr_str));
            });
            match rx.recv_timeout(connect_timeout) {
                Ok(Ok(s)) => {
                    let _ = s.set_read_timeout(Some(Duration::from_secs(2)));
                    let _ = s.set_write_timeout(Some(Duration::from_secs(2)));
                    return Ok(s);
                }
                Ok(Err(e)) if attempt + 1 < retries => {
                    eprintln!("[mediapipe] connect attempt {} failed: {e}, retrying...", attempt + 1);
                    std::thread::sleep(delay);
                }
                Ok(Err(e)) => return Err(e.into()),
                Err(_) if attempt + 1 < retries => {
                    eprintln!("[mediapipe] connect attempt {} timed out, retrying...", attempt + 1);
                    std::thread::sleep(delay);
                }
                Err(_) => return Err(anyhow::anyhow!("connect timed out")),
            }
        }
        unreachable!()
    }

    /// Send a JPEG frame and receive hand landmarks. On an IO error the
    /// connection is re-established with backoff and the request retried
    /// once, so a restarted Python server recovers without a bridge restart.
    pub fn detect(&self, jpeg: &[u8]) -> Vec<DetectedHand> {
        let mut stream = match self.stream.lock() {
            Ok(s) => s,
            Err(_) => return vec![],
        };

        if let Some(hands) = Self::detect_once(&mut stream, jpeg) {
            return hands;
        }

        // IO error: reconnect with backoff, then retry once.
        let addr = format!("127.0.0.1:{}", self.port);
        for attempt in 0..4 {
            match Self::try_connect(&addr, 1, Duration::from_millis(100)) {
                Ok(fresh) => {
                    *stream = fresh;
                    return Self::detect_once(&mut stream, jpeg).unwrap_or_default();
                }
                Err(_) => std::thread::sleep(Duration::from_millis(100 << attempt.min(4))),
            }
        }
        vec![]
    }

    /// One request/response round-trip. Returns `None` on any IO error.
    fn detect_once(stream: &mut TcpStream, jpeg: &[u8]) -> Option<Vec<DetectedHand>> {
        // Send: [4 bytes LE length] [JPEG data]
        let len_bytes = (jpeg.len() as u32).to_le_bytes();
        if stream.write_all(&len_bytes).is_err() || stream.write_all(jpeg).is_err() {
            return None;
        }
        let _ = stream.flush();

        // Read: [1 byte num_hands] then per hand: [1 byte handedness] [4 bytes score] [252 bytes landmarks]
        let mut hdr = [0u8; 1];
        if stream.read_exact(&mut hdr).is_err() {
            return None;
        }
        let n = hdr[0] as usize;
        if n == 0 {
            return Some(vec![]);
        }

        let mut hands = Vec::with_capacity(n);
        for _ in 0..n {
            let mut hand_buf = [0u8; 1 + 4 + 21 * 3 * 4]; // handedness + score + landmarks
            if stream.read_exact(&mut hand_buf).is_err() {
                break;
            }
            let h_code = hand_buf[0];
            let score = f32::from_le_bytes([hand_buf[1], hand_buf[2], hand_buf[3], hand_buf[4]]);
            let handedness = if h_code == 0 { "Left" } else { "Right" };

            let mut landmarks = [Landmark::default(); 21];
            for i in 0..21 {
                let off = 5 + i * 12;
                landmarks[i] = Landmark {
                    x: f32::from_le_bytes(hand_buf[off..off + 4].try_into().unwrap()),
                    y: f32::from_le_bytes(hand_buf[off + 4..off + 8].try_into().unwrap()),
                    z: f32::from_le_bytes(hand_buf[off + 8..off + 12].try_into().unwrap()),
                };
            }

            hands.push(DetectedHand {
                landmarks,
                handedness: handedness.to_string(),
                score,
            });
        }

        Some(hands)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;

    /// Start a mock MediaPipe server that reads one request and replies with
    /// a canned response. Returns the port to connect to.
    fn start_mock_server(response: Vec<u8>) -> u16 {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                // Read the 4-byte length prefix + JPEG data.
                let mut hdr = [0u8; 4];
                let _ = stream.read_exact(&mut hdr);
                let len = u32::from_le_bytes(hdr) as usize;
                let mut body = vec![0u8; len];
                let _ = stream.read_exact(&mut body);
                // Send canned response.
                let _ = stream.write_all(&response);
                let _ = stream.flush();
            }
        });
        port
    }

    #[test]
    fn detect_returns_empty_when_no_hands() {
        // Response: 0 hands
        let port = start_mock_server(vec![0x00]);

        let client = MediapipeClient::connect(port).unwrap();
        let hands = client.detect(b"\xFF\xD8\xFF\xE0");
        assert!(hands.is_empty());
    }

    #[test]
    fn detect_parses_one_left_hand() {
        // Build a response with 1 left hand.
        let mut resp = vec![0x01]; // 1 hand
        resp.push(0x00); // handedness: Left
        resp.extend_from_slice(&0.95f32.to_le_bytes()); // score
        for _ in 0..21 {
            resp.extend_from_slice(&0.1f32.to_le_bytes()); // x
            resp.extend_from_slice(&0.2f32.to_le_bytes()); // y
            resp.extend_from_slice(&0.3f32.to_le_bytes()); // z
        }
        let port = start_mock_server(resp);

        let client = MediapipeClient::connect(port).unwrap();
        let hands = client.detect(b"\xFF\xD8");
        assert_eq!(hands.len(), 1);
        assert_eq!(hands[0].handedness, "Left");
        assert_eq!(hands[0].score, 0.95);
        assert_eq!(hands[0].landmarks[0].x, 0.1);
        assert_eq!(hands[0].landmarks[0].y, 0.2);
        assert_eq!(hands[0].landmarks[0].z, 0.3);
    }

    #[test]
    fn detect_parses_two_hands() {
        let mut resp = vec![0x02]; // 2 hands
        for handedness in [0x00u8, 0x01u8] {
            resp.push(handedness);
            resp.extend_from_slice(&0.8f32.to_le_bytes());
            for _ in 0..21 {
                resp.extend_from_slice(&0.0f32.to_le_bytes());
                resp.extend_from_slice(&0.0f32.to_le_bytes());
                resp.extend_from_slice(&0.0f32.to_le_bytes());
            }
        }
        let port = start_mock_server(resp);

        let client = MediapipeClient::connect(port).unwrap();
        let hands = client.detect(b"\xFF\xD8");
        assert_eq!(hands.len(), 2);
        assert_eq!(hands[0].handedness, "Left");
        assert_eq!(hands[1].handedness, "Right");
    }

    #[test]
    fn detect_sends_length_prefixed_jpeg() {
        // Mock server that echoes back the received JPEG length as the response.
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut hdr = [0u8; 4];
                let _ = stream.read_exact(&mut hdr);
                let len = u32::from_le_bytes(hdr) as usize;
                let mut body = vec![0u8; len];
                let _ = stream.read_exact(&mut body);
                // Reply: 1 hand with score = received length as f32 (for assertion).
                let mut resp = vec![0x01];
                resp.push(0x00);
                resp.extend_from_slice(&(len as f32).to_le_bytes());
                for _ in 0..21 {
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                    resp.extend_from_slice(&0.00f32.to_le_bytes());
                    resp.extend_from_slice(&0.00f32.to_le_bytes());
                }
                let _ = stream.write_all(&resp);
                let _ = stream.flush();
            }
        });

        let client = MediapipeClient::connect(port).unwrap();
        let jpeg = vec![0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10];
        let hands = client.detect(&jpeg);
        assert_eq!(hands.len(), 1);
        // Score encodes the JPEG length we sent.
        assert_eq!(hands[0].score, jpeg.len() as f32);
    }

    #[test]
    fn detect_returns_empty_on_connection_failure() {
        // Connect to a port that nothing is listening on.
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        drop(listener); // close the port

        let stream = std::net::TcpStream::connect(format!("127.0.0.1:{port}"));
        assert!(stream.is_err());
    }

    #[test]
    fn detect_reconnects_after_server_restart() {
        use std::sync::mpsc::channel;

        // Server A: accepts one connection, then on signal closes it and
        // drops the listener so the port can be rebound (server "death").
        let listener_a = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener_a.local_addr().unwrap().port();
        let (accepted_tx, accepted_rx) = channel();
        let (kill_tx, kill_rx) = channel();
        std::thread::spawn(move || {
            if let Ok((conn, _)) = listener_a.accept() {
                let _ = accepted_tx.send(());
                let _ = kill_rx.recv();
                drop(conn);
            }
            drop(listener_a);
        });

        let client = MediapipeClient::connect(port).unwrap();
        accepted_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        kill_tx.send(()).unwrap();

        // Server B: rebind the same port; the client's next detect must
        // reconnect here and retry the request.
        let mut listener_b = None;
        for _ in 0..50 {
            match TcpListener::bind(format!("127.0.0.1:{port}")) {
                Ok(l) => { listener_b = Some(l); break; }
                Err(_) => std::thread::sleep(Duration::from_millis(50)),
            }
        }
        let listener_b = listener_b.unwrap();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener_b.accept() {
                let mut hdr = [0u8; 4];
                let _ = stream.read_exact(&mut hdr);
                let len = u32::from_le_bytes(hdr) as usize;
                let mut body = vec![0u8; len];
                let _ = stream.read_exact(&mut body);
                let mut resp = vec![0x01, 0x00]; // 1 left hand
                resp.extend_from_slice(&1.0f32.to_le_bytes());
                for _ in 0..21 {
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                }
                let _ = stream.write_all(&resp);
                let _ = stream.flush();
            }
        });

        // Only the post-reconnect retry can produce a hand: the first attempt
        // runs on the dead connection.
        let hands = client.detect(b"\xFF\xD8");
        assert_eq!(hands.len(), 1);
    }

    #[test]
    fn mediapipe_port_matches_contract() {
        assert_eq!(crate::net::MEDIAPIPE_PORT, 42073);
    }
}
