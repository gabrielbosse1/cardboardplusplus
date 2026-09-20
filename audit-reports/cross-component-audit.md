# Cross-Component Integration & Data-Flow Audit
Research only. No code changed. Date: 2026-09-19.
NOTE: agent output was truncated; F1-F10 intro missing. Preserved F11-F25 + minor notes as returned.

## End-to-end flow traced (summary)
- Video 42069: driver → phone BE length-prefixed + driver → localhost raw Annex-B for bridge ffmpeg preview (scale 480:270 RGBA).
- Discovery/control 42070: CARDBOARD_DISCOVERY/CAP/ACK phone↔driver; BRIDGE_HELLO/ACK/STATS/CFG/PREVIEW bridge↔driver.
- Telemetry 42071: 0x10 gyro 45B, 0x11 hand 15B, 0x12 rotation 25B quat [w,x,y,z], 0x13 netstats 21B, 0x20 ping, PHONE_HELLO.
- Camera 42072: [u16 seq BE]+JPEG 256x192 q38.
- MediaPipe 42073 TCP [u32 len LE][JPEG] → hands; 42074 bridge→driver sensors; 8567 REST; SHM protocol.rs ↔ BridgeProtocol.h natural alignment.

### F11 GetPose pose-field + sine fallback
- Files: driver HmdDriver.cpp:243-310. Assign fused quat to field SteamVR consumes (likely qDriverFromHeadRotation), keep qRotation in sync, remove sine-bob (hold last or invalid).

### F12 Video watchdog reconnect no-op while discovery runs
- VrActivity.java:137 reconnect=startDiscovery; DiscoveryManager.java:59-62 early-return if alive; broadcastUntilAck:100 loops while(broadcasting) forever. Watchdog VideoWatchdog.java:87-96 fires no-op every 3s stall. MED.
- Fix: DiscoveryManager.pokeNow() single discovery+CAP on live socket; watchdog calls that.

### F13 Stale-timeout ladder disagrees (2s vs 4s vs 5s)
- Driver sensor 2s (HmdDriver.cpp:260), bridge phone 4s (phone.rs:27), bridge driver-ACK 5s (driver.rs:24), driver phone-discovery 5s (Discovery.cpp:109); netstats/video watchdog 3s each.
- Fix: one ladder in CardboardWire.h (stale 2s suspect, 5s gone) + contract tests. MED.

### F14 Two clock domains port 42071 decorative but comparable
- TelemetrySender.java:263 boot-ms vs NetStatsReporter.java:110 wall-clock; bridge forwards untouched (phone.rs:184-216); driver uses steady_clock (HmdDriver.cpp:629,650). LOW (latent HIGH if latency computed).
- Fix: document domains in CardboardWire.h + telemetry.rs; prefer boot-ms.

### F15 BRIDGE_STATS partial parse poisons UI + ACK/STATS asymmetry
- driver.rs:129-136,145-163 ACK trim_start vs STATS raw; parse_stats Some((0,0,0,0)) on bare BRIDGE_STATS overwrites live fps. LOW.
- Fix: None when zero fields parsed; trim both.

### F16 No version negotiation despite strings
- BRIDGE_HELLO/ACK v1 (driver.rs:83, CardboardWire.h:40), PHONE_HELLO vN (TelemetrySender:154, telemetry.rs:131-135 starts_with only). No side compares. MED.
- Fix: parse+log versions; /status exposes driver_wire_v/phone_wire_v; warn/refuse major mismatch.

### F17 Wire constants triplicated; Android scattered/incomplete
- CardboardWire.h (42069/42070/42074 + strings) vs net/mod.rs:13-23 (ports only) vs AppConstants.java:12-16 (only 42069-42072, no 42073/42074/strings/tags). DiscoveryManager:30-32, TelemetrySender:41-42, NetStatsReporter:30-31, VideoReceiver.h:64 re-hardcode. AGENTS.md:415-420 claims 3 files suffice — actually 8+. MED.
- Fix: wire.toml/json root + codegen or checked-in generated constants; centralize Android into AppConstants; contract tests assert schema. Minimum: centralize.

### F18 Bridge decodes every camera JPEG even when dropped
- camera.rs:105-145 decode before tx.try_send; overlay-on skips raw store yet still decodes; channel depth 2 full → decoded RGBA discarded @30fps. MED.
- Fix: try_send readiness probe first; skip decode when overlay-on AND channel full; SOI check only.

### F19 JPEG decoded twice (bridge + sidecar) — justified but unbounded
- Bridge zune-jpeg (camera.rs:119) + Python cv2.imdecode (mediapipe_server.py:116); TCP sends JPEG correct (RGBA 6x bytes). detect() blocks up to 2s (mediapipe.rs:57-58,124) holding single thread — wedged sidecar starves display when overlay on (sole writer camera.rs:204-207). LOW.
- Fix: keep dual; add frame-age drop (>300ms skip detect); overlay-off fallback so display never starves.

### F20 0x10 forwarding pure overhead
- Bridge forwards every 0x10 ~200Hz to 42074 (phone.rs:145-147,184-200); driver stores (HmdDriver.cpp:624-630) but GetPose reads only m_sensorQuat; validity m_lastSensorRecvMs refreshed by both so 0x10 masks 0x12 outages. MED.
- Corrected fix (see fix-risks.md R7): forward 0x12 always, 0x10 throttled ~10Hz for diagnostics; split validity (rotation-fresh primary, gyro fallback for gyro-only devices + log path). Do NOT drop 0x10 entirely.

### F21 Video MTU 60KB over WiFi
- Driver 60000B (UdpTransport.cpp:31), phone 65536 (VideoReceiver.h:46), bridge 65535. 60KB >>1500 → ~40 fragments; one lost = datagram lost; length-prefix desync → buffer.clear + KEYFRAME_REQ throttled 500ms (VideoReceiver.cpp:183-190,111-131). Burst loss = prolonged corruption. MED.
- Fix cheapest-first: per-chunk seq so single loss drops one frame cleanly; smaller chunks ~1400B; later keyframe-preferential retransmit; document assumption.

### F22 No backpressure/sequence bridge→driver; phone→bridge no seq
- 42074 fire-and-forget (phone.rs:17); 42071 no seq — bridge can't loss-vs-silence; gyro_fps pill (app.rs:172-188) conflates. LOW.
- Fix: per-tag seq byte (major wire change — with F16 versioning) or count parse-OK vs Unknown proxy.

### F23 Setup breakage inventory
- NAT/firewall: 42070 must be bidirectional; DiscoveryManager.java:126-132 learned broadcast CAP dropped by Windows Firewall (unicast on proven socket). Telemetry/camera/netstats ride getPcOrBroadcastAddress — broadcast spams LAN when no PC IP. MED. Fix prefer unicast after any ACK; document rules.
- Tailscale vs LAN: 255.255.255.255 never crosses Tailscale — must set PC IP manually (AppSettings.pcIp default ""), no UI prompt. MED. Fix "no ACK → enter PC IP" hint both sides.
- Localhost preview: driver→127.0.0.1:42069 (UdpTransport.cpp:89-91), bridge binds 127.0.0.1:42069 (core.rs:608). Remote/second bridge gets nothing/collides silently (core.rs:608-613). LOW. Fix log + preview-target config.
- IPv6: all AF_INET (Discovery.cpp:25, HmdDriver.cpp:512, VideoReceiver.cpp:31); NetworkUtils may return v6 → opaque fail. LOW. Validate IPv4 literal.
- SteamVR restart: in-process state; install-driver.ps1:41 warns correctly; no stale-DLL detection (see F16). LOW.
- ADB: install-app.ps1:26-28 USB only; AGENTS.md adb connect over Tailscale but no script path. LOW. Add -PhoneIp → adb connect.

### F24 Installer/deploy gaps
- Driver ffmpeg: install-driver.ps1:29-31 silent skip if dir missing; no hash; single .bak. MED.
- Bridge ffmpeg: core.rs:636-639 logs once, preview dead no UI. MED. Fix ffmpeg -version preflight in /status.
- Python sidecar: no dep pin/model install; lazy retry (camera.rs:177-192, mediapipe.rs:101-109) masks missing as "0 hands" indefinitely. HIGH vs ship-together rule. Fix health-check at startup (connect_healthy probe exists), expose hand_pipeline_ok in UI/REST, warn loudly but keep stream running (see fix-risks.md R12).
- APK skew: installs app-debug.apk no version assert; with F16 all three can be different vintages silently. MED.

### F25 Duplicate heartbeat/polling loops
- Phone discovery 500ms forever + driver phone-timeout 5s + bridge phone 4s + bridge heartbeat 500ms + driver sensor 2s + phone watchdog 500ms poll + netstats 2s — all infer alive from overlapping streams. Phone eternal broadcast compensates for no persistent phone registry — which bridge already has (phone_ip/phone_connected). LOW.
- Corrected (see fix-risks.md R1): back off phone discovery to 1 per 5s after ACK instead of stopOnAck; bridge-broker stays fallback, never sole path — driver restart must still rendezvous directly.

## Minor
- SHM names cbpp::kRegionName (HmdDriver.cpp:485) vs protocol.rs:73-75 NAME_PREFIX — needs byte-level name test (follows F17).
- EncoderChoice::from(i32) indices 2-4 no UI source; stale comment if picker 0/1-only.
- VideoManager 1920x1080 hardcoded (VideoManager.java:48-49, AppConstants:41-42) vs encoder 2880x1620 — heals via SPS reconfigure (VideoDecoder:195-208) but first-frame latency + needless reconfig. Init from AppSettings.
- DecoderCapabilityReporter max-W + max-H may come from different decoders — announce combo no single decoder supports. Take per-decoder max-area / min-of-uppers.

## Top fixes
1. Resolution latch/race (driver) + bridge discipline.
2. Silent hand-pipeline absence + no versioning (ship-together unenforceable).
3. Discovery hijack + dead watchdog.
4. Drop 0x10 forwarding + skip doomed JPEG decodes (free CPU/pps).
5. Wire schema (stops triplication drift).
