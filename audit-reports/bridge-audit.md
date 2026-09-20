# Bridge Audit — `bridge/`
No code changed. Research only. Date: 2026-09-19.

## Files checked (33)
protocol.rs, lib.rs, ring.rs, mem.rs, bridge-core shm.rs/paths.rs, cardboard-bridge main.rs/lib.rs/app.rs/core.rs/hand_overlay.rs/build.rs, net/mod.rs/driver.rs/phone.rs/telemetry.rs/camera.rs/mediapipe.rs, server.rs/handlers.rs/index.rs, tests mock_phone.rs/mock_driver.rs/hand_sidecar.rs, ui/app.slint (1-120), bridge-ui main.rs, scripts/compile-bridge.ps1 + compile-all.ps1.

## 1. Incorrect comments
- F1 protocol.rs:3, lib.rs:15 reference docs/TRANSPORT.md which doesn't exist. Fix: point at protocol.rs.
- F2 telemetry.rs:10-13 "drives adaptive bitrate" false; app.rs:215-220 manual-Apply only. Fix reword.
- F3 server/index.rs:26 vs handlers.rs:100-106 "missing keep defaults" wrong — falls back to APPLIED_DEFAULTS 2880x1620@60 20Mbps. POST /settings {} resets session. HIGH.
- F4 AGENTS.md POST /preview doesn't exist; handlers.rs:25-35 only GET. Fix docs.
- F5 mem.rs:69-70 "fails if exists" wrong on Windows CreateFileMappingW opens existing. Fix comment.
- F6 phone.rs:17 ponytail comment points at nonexistent CmdProducer PublishPose.

## 2. Dead code
- F7 phone.rs:170-177 send_to_phone dead + binds new socket per call. Delete.
- F8 telemetry.rs:152-162 encode_hello/encode_ack dead + encode_ack wrong (bare BRIDGE_ACK vs BRIDGE_ACK v1). Delete or fix.
- F9 Shipped binary never reads SHM — cardboard-bridge imports only paths; zero ShmService/BridgeConsumer refs. SHM read only in bridge-ui binary which compile-bridge never launches. HIGH — driver SHM read by nobody in product config. Fix: spawn ShmService::drain in AppCore::new or document UDP-only.
- F10 app.rs:46,179 + main.rs:228 hand_fps computed never displayed (UI binds camera_fps). Fix bind or delete.
- F11 app.rs:42-43 stream_fps/latency_ms write-never zeros → UI+REST permanent 0. Drive from preview_driver_fps or delete.
- F12 bridge/bridge/ stray nested duplicate workspace. Delete bridge/bridge/.
- F13 mock_driver.rs:14-17 unused helper + test binds real 42070. Bind 127.0.0.1:0 or delete.
- F14 camera.rs:60 detect_running never false. Remove flag.

## 3. Redundant work
- F15 main.rs:207-269 20Hz full snapshot (status lock + 5 clones, 9 formats, logs(200).join). Split stats 10-20Hz, log_text 1-2Hz.
- F16 camera.rs:132,144 two copies per frame (rgba.clone + jpeg to_vec @30fps). Use Arc<Vec<u8>>.

## 4. Library vs hand-rolled
- F17 tiny_http single-threaded (server.rs:32-36) — one slow /status wedges /health. Spawn per request or axum/hyper if routes grow.
- F18 manual arg/env, ring log, netstat FFI — keep (ladder), but netstat locale-fragile.
- F19 hand_overlay raster ~45 lines — keep.

## 5. Round-trips
- F20 42074 re-serializes instead of relaying (phone.rs:184-216) — parse 9x/4x f32 then rebuild identical buffer + Vec per packet. Fix: validate len==45&&tag==0x10 || len==25&&tag==0x12, then send_to(raw_datagram) untouched (see fix-risks.md R10).
- F21 camera double-handle RGBA decode + JPEG forward per frame; opt-level=3 hack for zune-jpeg. Arc-share RGBA; skip decode when sidecar unreachable + overlay off + nobody watches.
- F22 MediaPipe TCP single Mutex<TcpStream> serializes detect + set_config with 2s timeouts. Separate connection for set_config or IO thread + queue.

## 6. Perf
- F23 per-packet Vec 42074 @~1kHz (phone.rs:185,207). Stack [u8;45]/[u8;25].
- F24 driver-conn mutex held across blocking recv_from 200ms (driver.rs:96,189-197) → Apply stalls. Separate sockets or try_lock + dedicated send socket.
- F25 preview per-frame Vec ~518KB RGBA (core.rs:650-666,682-686) + 1ms busy drain. Double-buffer reuse; backoff 2-5ms or blocking recv+timeout.
- F26 ring-log drain memmove O(200) — keep or VecDeque.

## 7. Logic bugs
- F27 HIGH: handle_bridge_datagram STATS returns Stop (driver.rs:129-137) → ACK behind STATS waits 500ms; only ACK refreshes last_ack → live driver flaps driver_connected on dropped ACKs. Fix: return Continue for STATS, refresh liveness only if src == 127.0.0.1:DRIVER_DISCOVERY_PORT (poll_for_ack ignores _src today — see fix-risks.md R4).
- F28 parse_stats unwrap_or(0) → malformed = (0,0,0,0) indistinguishable from idle. Reject packet, count rejects.
- F29 garbage keeps phone connected (phone.rs:60-64 last_seen before parse; Unknown refreshes liveness). Fix: stamp last_seen on Hello/Ping/Gyro/Hand/Rotation/NetStats, ignore Unknown only — keep Ping as keepalive (see fix-risks.md R6).
- F30 phone_ip only on gyro/rotation/hello (phone.rs:90-134); Hand/NetStats/Ping don't update → stale after DHCP roam. Fix: keep IP update on Hello/Gyro/Rotation only; NetStats/Hand update only if already connected and src matches current; never on Ping/Unknown (Ping is 1-byte forgeable — see fix-risks.md R5).
- F31 unconditional ROT_DEBUG eprintln every 20th rotation ~25-50 lines/s, not gated by debug_enabled (phone.rs:125-132). Gate or delete.
- F32 42072 seq parsed never used (camera.rs:111,133-140) — last-received not newest; reorder shows old over new. Track last_seq wrapping-aware; drop stale; count gaps. BE correct — do NOT unify.
- F33 detect_once partial hands as success (mediapipe.rs:215-217) → torn TCP = 1/2 hands no error. Return None on mid-reply error.
- F34 ShmService::drain double-counts drops (shm.rs:43-52 + ring.rs). Track delta.
- F35 REST /logs?n= only bare n= (handlers.rs:63-68). Split &.
- F36 ACK trim_start vs STATS raw starts_with asymmetry. Trim both or strict both.

## 8. Fallback defaults
- F37 HIGH: POST /settings partial resets to compiled defaults (handlers.rs:100-106). Default None to live applied_*/encoder; expose applied resolution/fps in snapshot (only bitrate exposed today).
- F38 Encoder from(i32) unknown→Gpu, from_name typo→Auto silently (net/mod.rs:41-67). Log fallback.
- F39 preview 480x270 hardcoded in two places (core.rs:618-647) + AGENTS.md probesize 32 vs code 32768. Single const PREVIEW_W/H + format! vf string.

## 9. Packaging
- F40 HIGH: compile-bridge.ps1 ships exe only. core.rs:192-229 searches for mediapipe_server.py; model at models/hand_landmarker.task never copied. Add install-bridge.ps1 copy sidecar+model next to exe; assert presence.
- F41 HIGH: no requirements.txt; spawn PYTHON or "python" (absent here; Windows needs py -3); imports cv2/numpy/mediapipe.tasks unpinned. Add requirements + probe py -3 + log version.
- F42 HIGH: ffmpeg assumed on PATH (core.rs:618-640), absent here; preview pill 0 forever. Startup check + winget note or vendor static build.
- F43 slint split 1.17 vs 1.10 (cardboard-bridge vs bridge-ui). Pin via workspace.dependencies.
- F44 no bridge installer script. Add install-bridge.ps1 (exe/sidecar/model, ffmpeg/python check, firewall).

## 10. Setup breakage
- F45 firewall: nothing provisions 42069-42074/8567. Add inbound UDP 42071/42072 rules; hint when bound but zero packets 10s.
- F46 preview binds 127.0.0.1:42069 no retry (core.rs:608-615,717-740). Retry with backoff; surface PID-in-use via listen_pid.
- F47 IPv4 literals; localhost→::1 over VPN edge. Document.
- F48 tests squat fixed ports (mock_driver 42070, hand_sidecar 42073/42072). Ignore 42070 test or bind :0; document stop-bridge-before-test.
- F49 REST loopback-only by design? (server.rs:16-28). Document as security choice; add auth before any 0.0.0.0.

No Tokio — stdlib threads throughout (matches UDP rationale). Endianness: 42071/42073/42074 LE; 42072 seq BE u16 correct; 42069 phone BE length-prefix, localhost raw Annex-B.
Order: F27 → F37 → F40/F41/F42 → F9 → F20/F23.
