# Bridge Agent — Performance + Logic Audit

Scope: `bridge/` only (Rust workspace).
Files scanned: 28 .rs (27 unique +1 stale duplicate), 5 Cargo.toml, 1 .py, 2 .slint, 1 C header mirror.
Findings: 6 critical, 19 major, 30 minor.

Note: Rust `Pose` is 88 bytes with repr(C) padding (not 84). C static_assert(==88) matches reality. Fix the Rust doc comment.

Transport vs AGENTS.md: compliant (42069 UDP video, 42070 UDP heartbeat, 42071 UDP latest-wins, 42072 UDP frame-per-datagram, 42073 TCP req-resp, 42074 UDP forward). Violation is structural: settings travel twice (UDP BRIDGE_CFG + SHM SETTINGS) with no winner.

## Critical

1. `bridge/crates/bridge-core/src/d3d11.rs:101-112` | critical | logic | Staging copies SampleDesc with MSAA — illegal, CreateTexture2D fails every frame. Fix: force Count:1 + ResolveSubresource if source Count>1.
2. `bridge/crates/bridge-shm/src/mem.rs:210-219` | critical | logic | Linux Drop unlinks status region even when handle owns cmd region. Fix: store region name in handle, unlink that.
3. `bridge/crates/cardboard-bridge/src/core.rs:154-180` | critical | logic | MediaPipe child stdout piped never drained, 64KB pipe wedges child. Fix: Stdio::null() or drain thread.
4. `bridge/crates/cardboard-bridge/src/core.rs:188-198` | critical | logic | Child handle dropped immediately, python never killed on exit. Fix: store Child, kill on shutdown.
5. `bridge/crates/cardboard-bridge/src/net/mediapipe.rs:103-154` | critical | logic | detect() empty on IO error, never reconnects — zero hands until restart. Fix: reconnect with backoff.
6. `bridge/crates/bridge-ui/src/main.rs:571-583` | critical | logic | install_driver deletes live DLL before copy; failed copy = no DLL. Fix: copy to temp then rename.

## Major

7. `bridge/crates/cardboard-bridge/src/net/driver.rs:99-101` | major | perf | poll_for_ack holds DRIVER_CONN mutex across 200ms recv, stalls send_config. Fix: try_clone socket per thread.
8. `bridge/crates/bridge-core/src/shm.rs:43-52` | major | logic | dropped_total += cumulative counter double-counts. Fix: `dropped_total = consumer.dropped()`.
9. `bridge/crates/cardboard-bridge/src/core.rs:434-439` | major | perf | Fresh 518KB Vec per preview frame at 60fps (~30MB/s churn). Fix: reuse two buffers, swap.
10. `bridge/crates/cardboard-bridge/src/net/camera.rs:122-131` | major | perf | rgba.clone() (196KB) + jpeg to_vec per frame (~7MB/s). Fix: move owner or Arc.
11. `bridge/crates/cardboard-bridge/src/net/camera.rs:169-186` | major | perf | 4-bytes-per-pixel push loop (~200k pushes). Fix: resize + row slice copy.
12. `bridge/crates/cardboard-bridge/src/server.rs:32-36` | major | perf | tiny_http single-thread, slow client stalls /status. Fix: thread per request.
13. `bridge/crates/cardboard-bridge/src/main.rs:95-97` | major | perf | State poller 50ms vs documented 250ms, clones + formats 9 floats + joins 200 logs at 20Hz. Fix: 250ms, skip unchanged.
14. `bridge/crates/cardboard-bridge/src/app.rs:14` | major | perf | Single Arc<Mutex<AppState>> locked per telemetry packet (~1kHz) + UI + REST. Fix: RwLock or atomics.
15. `bridge/crates/bridge-ui/src/main.rs:626-649` | major | logic | spawn_start_steamvr ignores cfg.steamvr_drivers_dir, uses hardcoded DEFAULT. Fix: pass cfg dir in.
16. `bridge/crates/cardboard-bridge/src/net/driver.rs:212-220` + `bridge/crates/bridge-core/src/shm.rs:69-91` | major | logic | Two settings channels (UDP BRIDGE_CFG, SHM SETTINGS) no precedence. Fix: pick canonical, document.
17. `bridge/crates/bridge-shm/src/mem.rs:71-92` | major | logic | Windows create() never checks ALREADY_EXISTS, two producers coexist. Fix: check GetLastError.
18. `bridge/crates/bridge-shm/src/mem.rs:131-152` | major | logic | Linux create() O_CREAT without O_EXCL + ftruncate truncates live region. Fix: O_EXCL.
19. `bridge/crates/cardboard-bridge/src/net/driver.rs:133-141` | major | logic | BRIDGE_STATS returns Stop, never refreshes last_ack — ACK behind STATS waits 500ms. Fix: Continue + refresh liveness.
20. `bridge/crates/cardboard-bridge/src/core.rs:262-293` + `src/server/handlers.rs:111-150` | major | logic | REST /settings zero validation, negatives = garbage wire packets. Fix: clamp + 400.
21. `bridge/crates/cardboard-bridge/src/net/phone.rs:179-211` | major | perf | Fresh Vec + format!("127.0.0.1:...") per gyro sample. Fix: stack [u8;45]/[u8;25] + cached SocketAddr.
22. `bridge/crates/cardboard-bridge/Cargo.toml:8` vs `bridge/crates/bridge-ui/Cargo.toml:16` | major | perf | Two Slint versions (1.17 + 1.10) double UI build. Fix: unify.
23. Multiple files | major | logic | No graceful shutdown: loop{}, children not killed, SHM leaks. Fix: shutdown flag + kill children.
24. `bridge/crates/bridge-shm/src/ring.rs:112-127` | major | logic | Overwrite next=ws skips newest, corrupt slot stalls drain. Fix: jump ws-1 when valid, distinguish empty vs corrupt.
25. `bridge/crates/bridge-core/src/d3d11.rs:87-119` | major | perf | OpenSharedResource + CreateTexture2D per eye per frame. Fix: cache textures + staging per (w,h,fmt).

## Minor

26. `bridge/crates/cardboard-bridge/src/net/driver.rs:61-63,86,100,180,216` | minor | logic | Six .expect() on mutexes/addr, poison panics heartbeat. Fix: unwrap_or_else into_inner.
27. `bridge/crates/bridge-shm/src/protocol.rs:138-149` | minor | logic | Pose doc says 84, real 88. Fix: comment to 88.
28. `bridge/crates/cardboard-bridge/src/app.rs:115-121` | minor | perf | log.drain(0..excess) memmoves 200 lines per overflow. Fix: VecDeque.
29. `bridge/crates/cardboard-bridge/src/net/telemetry.rs:1-11` | minor | logic | Docs omit 0x12, AGENTS.md lacks 0x12 + 42074. Fix: update both.
30. `bridge/crates/cardboard-bridge/src/net/driver.rs:149-167` | minor | logic | parse_stats None vs Some(0) ambiguous. Fix: skip malformed fields.
31. `bridge/crates/cardboard-bridge/src/net/phone.rs:20-22` | minor | logic | sensor_sock() expect panics telemetry thread. Fix: return Option.
32. `bridge/crates/cardboard-bridge/src/net/phone.rs:125-132` | minor | perf | eprintln every 20th rotation on hot path. Fix: gate debug_enabled.
33. `bridge/crates/cardboard-bridge/src/net/phone.rs:105-108,119-123` | minor | perf | src.ip().to_string() alloc per packet. Fix: compare IpAddr before alloc.
34. `bridge/crates/cardboard-bridge/src/net/phone.rs:71-73` | minor | logic | Hard errors retry no sleep, 100% CPU spin. Fix: 50ms sleep.
35. `bridge/crates/cardboard-bridge/src/server/handlers.rs:101-107` | minor | logic | ?n= only bare prefix, n=50&x=1 falls back. Fix: proper query parse.
36. `bridge/crates/cardboard-bridge/src/server/handlers.rs:58-90` | minor | logic | POST /preview {} 200 no-op, empty 400. Fix: 400 on no-op or document.
37. `bridge/crates/cardboard-bridge/src/core.rs:228,310-327,456-500` | minor | logic | .expect("state lock") panics UI on poison. Fix: fallback defaults.
38. `bridge/crates/cardboard-bridge/src/core.rs:396-415` | minor | perf | Preview feeder 1ms spin (1000 wakeups/s). Fix: blocking recv 100ms.
39. `bridge/crates/cardboard-bridge/src/core.rs:468-507` | minor | perf | Arc<Arc<AppCore>> leak per start + 500ms poll. Fix: Weak + oneshot.
40. `bridge/crates/cardboard-bridge/src/core.rs:511-532` | minor | perf | stop_preview child.wait() on UI/REST. Fix: background wait.
41. `bridge/crates/cardboard-bridge/src/core.rs:538-561` | minor | logic | ffplay never killed on exit. Fix: kill on shutdown.
42. `bridge/crates/bridge-ui/src/main.rs:29,531-560` | minor | logic | Hardcoded VS18 MSBuild path. Fix: vswhere.
43. `bridge/crates/bridge-ui/src/main.rs:83-94` | minor | logic | Corrupt bridge.json overwritten with defaults. Fix: backup corrupt first.
44. `bridge/crates/bridge-ui/src/main.rs:572-576` | minor | logic | Backup timestamp 1s resolution collides. Fix: add PID/nanos.
45. `bridge/crates/bridge-ui/src/main.rs:614-624` | minor | logic | install_apk no adb timeout. Fix: timeout + devices check.
46. `bridge/crates/bridge-core/src/d3d11.rs:145` | minor | logic | Unknown DXGI assumes 4Bpp, mis-copies. Fix: return error.
47. `bridge/crates/cardboard-bridge/src/net/camera.rs:73-99`, `src/net/phone.rs:32-51`, `src/net/driver.rs:45-67`, `src/core.rs:352-360` | minor | perf | No SO_RCVBUF tuning 42069-42072. Fix: 1-4MB on camera/telemetry.
48. `bridge/crates/cardboard-bridge/src/net/camera.rs:169-186` | minor | logic | Assumes RGB, misrenders gray JPEG. Fix: check pixel_format, expand gray.
49. `bridge/crates/cardboard-bridge/tests/mock_driver.rs:75-93` | minor | logic | Test binds real 42070, flaps with bridge. Fix: port 0 / skip if taken.
50. `bridge/bridge/crates/bridge-shm/examples/sizeprobe.rs` | minor | logic | Stale duplicate bridge/bridge/. Fix: delete duplicate.
51. `bridge/crates/cardboard-bridge/tests/mock_driver.rs:158-166` | minor | logic | port_constants test omits 42074. Fix: assert 42074.
52. `bridge/crates/cardboard-bridge/src/net/mediapipe.rs:40-56,69-100` | minor | perf | 10 linger threads per connect() on hang. Fix: single thread + deadline.
53. `bridge/crates/cardboard-bridge/src/main.rs:43-48` | minor | logic | Headless 3600s sleep, no Ctrl+C. Fix: signal/condvar + shutdown.
54. `bridge/crates/cardboard-bridge/src/net/mediapipe.rs:121-126` | minor | logic | Hand count u8 trusted for capacity/loop. Fix: clamp n to 2.
55. `bridge/crates/cardboard-bridge/src/net/mediapipe.rs:27,134,146-149` | minor | perf | handedness String alloc per hand per frame. Fix: bool/enum.
