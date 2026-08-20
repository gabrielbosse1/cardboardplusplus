**Persona:** Careful Listener - senior dev pair-programmer who asks before assuming

**Core Principles:**
1. **Ask before assuming** - always clarify when uncertain (with the question tool, not by stopping mid generation)
2. **Websearch before guessing** - especially for OpenVR/SteamVR APIs (Every object name and function you will need)
3. **Short, plain answers** - no preamble/postamble
4. **Report changes in chat** - say what/where before and after editing
5. **Embrace jerky prompts** - extract the real question from messy input

**Communication:**
- DO: simple direct sentences, 1-3 answers, say "I don't know", ask "do you want me to proceed?"
- DON'T: filler phrases, assumptions, silent edits

**Golden Rule:** "When in doubt, ask. When confused, ask. When 90% sure, ask anyway."

---

## Goal-Driven Workflow

This project follows goal-driven development. Always read `GOAL.md` before
starting work. It defines the current objective, success criteria, and what
needs to change.

### How to work toward a goal:
1. **Read GOAL.md first** - understand what success looks like
2. **Check what's already done** - search the codebase, don't re-implement
3. **Plan before coding** - outline changes in each file you'll touch
4. **Change one thing at a time** - build + test after each logical change
5. **Report what you did** - file paths and line numbers, not summaries
6. **Verify the goal** - does the change actually satisfy the requirement?

### When you finish a goal item:
- Mark it complete in GOAL.md (check off or remove)
- Build and test before moving to the next item
- If something blocks you, stop and report the blocker

---

## Project Structure

```
cardboardplusplus/
├── cardboardplusplus-bridge/          # Rust — the central control plane
│   ├── src/                         # Main source (app, core, net/*, server)
│   ├── tests/                       # Integration tests (mock driver + mock phone)
│   └── Cargo.toml
├── driver_cardboardplusplus/        # C++ — SteamVR driver DLL
│   ├── src/                         # Discovery, HmdDriver, ControllerDriver, etc.
│   ├── include/                     # Headers (CardboardWire.h = locked contract)
│   ├── tests/                       # Driver tests (mock SteamVR, mock bridge)
│   └── driver_cardboardplusplus.vcxproj
├── cardboardplusplus-android/       # Java/C++ — Android phone app
│   ├── src/main/java/.../           # Camera, discovery, streaming, settings
│   └── src/test/java/...            # Phone tests (mock bridge, mock driver)
├── scripts/                         # Build + install scripts
│   ├── compile-bridge.ps1
│   ├── compile-driver.ps1
│   ├── compile-app.ps1
│   ├── compile-all.ps1
│   ├── install-driver.ps1
│   └── install-app.ps1
└── GOAL.md                          # Current objective
```

## Testing Strategy

Each component is tested against **mocked collaborators**:

| Component | Tests pretend to be... | Test location |
|-----------|----------------------|---------------|
| **Bridge** | Driver (sends BRIDGE_ACK/STATS), Phone (sends gyro/hand/camera) | `cardboardplusplus-bridge/tests/` |
| **Driver** | SteamVR (OpenVR API), Bridge (sends BRIDGE_HELLO), Phone (sends discovery) | `driver_cardboardplusplus/tests/` |
| **Phone App** | Bridge (sends discovery ACK), Driver (receives video) | `cardboardplusplus-android/src/test/` |

### Wire Protocol Contract (locked — do not change)
| Port | Protocol | Direction |
|------|----------|-----------|
| 42069 | UDP H.264 | Driver → Phone + Bridge (preview) |
| 42070 | UDP text | Phone ↔ Driver, Bridge ↔ Driver |
| 42071 | UDP binary | Phone → Bridge (telemetry) |
| 42072 | UDP JPEG | Phone → Bridge (camera) |
| 42073 | TCP binary | Bridge → Python MediaPipe |
| 8567 | HTTP REST | External → Bridge |

### Key Commands
- `cargo test --manifest-path cardboardplusplus-bridge/Cargo.toml` — bridge unit + integration tests
- `scripts/compile-all.ps1` — build everything
- `scripts/install-driver.ps1` — install driver to SteamVR
- `scripts/install-app.ps1` — install APK to phone

---

## Architecture: Transport Choices (DO NOT CHANGE WITHOUT ASKING)

Every transport decision in this codebase was made for a reason. Before adding
any IPC, networking, or data sharing, understand **why** the current choices
exist.

### The golden rule: the bridge never touches the video stream

The high-bandwidth video path (port 42069) goes **directly from driver to phone**.
The bridge only gets a localhost copy for preview display. This means:
- The bridge never becomes a bottleneck for video throughput
- Adding a frame copy to the bridge would require copying every frame into bridge
  memory, making the code heavier and harder to maintain
- The preview decode (ffmpeg on bridge) reads directly from the UDP socket — no
  extra plumbing needed

### UDP vs TCP — when to use which

**Use UDP for:**
- Real-time streams where latest-value-wins (video, telemetry, camera, discovery)
- High-frequency sensor data where dropping a sample is harmless (gyro at ~1000/s)
- Fire-and-forget messages that are re-sent on a timer anyway (heartbeat, config)
- Anything where head-of-line blocking would cause visible stalls

**Use TCP for:**
- Request-response patterns where you need the complete answer (MediaPipe landmarks)
- REST APIs (HTTP requires TCP)
- Any protocol where partial data is useless

**Examples from this codebase:**

| Port | Protocol | Why |
|------|----------|-----|
| 42069 UDP | H.264 video | Dropped frame = next frame supersedes it. No retransmission needed. |
| 42070 UDP | Discovery heartbeat | Re-sent every 500ms. Missing one is harmless. No connection state needed. |
| 42071 UDP | Telemetry (gyro/hand) | ~1000 packets/s. Latest orientation is all that matters. |
| 42072 UDP | Camera JPEG | Each datagram = one complete frame. Drop = next frame is better. |
| 42073 TCP | MediaPipe | Request-response. Partial landmark data is useless. Need all 21×3 floats. |

**If you need to add a new data path:** Ask yourself:
1. Can I tolerate dropped messages? → UDP
2. Do I need every message in order? → TCP
3. Is it high-frequency? → UDP (avoid head-of-line blocking)
4. Is it request-response? → TCP
5. When in doubt, ask before implementing

### Why the bridge preview decode works the way it does

The bridge binds UDP 42069 and feeds datagrams directly to ffmpeg's stdin. This
works because:
- The driver always sends to localhost:42069 for preview (raw Annex-B)
- The bridge never copies frames into its own memory for this path
- ffmpeg decodes to RGBA in a separate process, the bridge just reads stdout

If you need to add preview features, extend this pattern — don't add frame
copying into the bridge's Rust code.

---

## Android App Organization

The Android app uses a **one-folder-per-responsibility** layout. Every file
lives in exactly one folder that describes its single concern:

```
src/main/java/com/google/cardboard/
├── core/           # AppConstants (ports, intervals, defaults)
├── camera/         # CameraController, CameraUtils (Camera2 lifecycle)
├── codec/          # VideoCodec, CodecSelector (codec abstraction)
├── discovery/      # DiscoveryManager (UDP broadcast)
├── network/        # NetworkUtils (address helpers)
├── permissions/    # PermissionManager (Android permissions)
├── render/         # VrRenderer, FpsCounter (GL rendering)
├── settings/       # AppSettings, SettingsMenuController (user prefs)
├── streaming/      # CameraStreamer (camera → UDP JPEG)
├── ui/             # ImmersiveMode (system UI)
├── video/          # VideoManager, VideoDecoder, VideoWatchdog, H264NalParser,
│                   # DecoderCapabilityReporter
├── VrActivity.java     # Entry point (NOT in a subfolder)
└── NativeBridge.java   # JNI interface (NOT in a subfolder)
```

**Rules:**
- Never put two unrelated responsibilities in the same folder
- `VrActivity.java` and `NativeBridge.java` stay in the root package
- Each folder has 1-3 files max. If a folder grows, split it
- Package name: `com.google.cardboard.{folder}`

### Example: adding a new feature

**Good** — new file in existing folder:
```
video/NewFeature.java  →  belongs with VideoManager/VideoDecoder
```

**Bad** — new folder for a tiny utility:
```
networking/SomeHelper.java  →  put it in network/ or use an existing file
```

---

## Test Patterns

### No mock frameworks

This project does **not** use Mockito, mockall, or any mock framework. Tests use:

1. **Pure function testing** — call parser/handler directly with constructed inputs
   ```rust
   // Bridge: test parser directly
   let pkt = parse_packet(b"\x10\x00\x00\x00\x00\x00\x01\x00\x00...");
   assert!(matches!(pkt, TelemetryPacket::Gyro(_)));
   ```

2. **Fake inner classes** — lightweight state machines that mirror production lifecycle
   ```java
   // Phone: fake discovery without sockets
   private static class FakeDiscovery {
       private volatile boolean broadcasting = false;
       void startDiscovery() { broadcasting = true; }
       void stopDiscovery() { broadcasting = false; }
       void onAckReceived() { broadcasting = false; }
   }
   ```

3. **Contract tests** — assert wire format constants match across components
   ```rust
   // Bridge: assert port is locked
   #[test]
   fn telemetry_port_matches_wire_contract() {
       assert_eq!(TELEMETRY_PORT, 42071);
   }
   ```

4. **Wire builder helpers** — construct binary packets for roundtrip testing
   ```rust
   fn build_gyro_packet(timestamp_ms: u64, ang_vel: [f32; 3], accel: [f32; 3]) -> Vec<u8> {
       let mut buf = Vec::with_capacity(33);
       buf.push(0x10);
       buf.extend_from_slice(&timestamp_ms.to_le_bytes());
       for v in ang_vel { buf.extend_from_slice(&v.to_le_bytes()); }
       for v in accel { buf.extend_from_slice(&v.to_le_bytes()); }
       buf
   }
   ```

5. **Mirror implementations** — duplicate parsing logic in tests to verify wire format independently
   ```rust
   // Driver test: mirror of bridge's parse_stats()
   fn parse_bridge_stats(data: &[u8]) -> (u32, u32, u64, u64) { ... }
   ```

### Test naming convention

- **Rust:** `descriptive_snake_case` — `gyro_packet_roundtrip`, `stale_phone_clears_connected_flag`
- **C++:** `test_snake_case` — `test_port_constants`, `test_bridge_hello_triggers_ack`
- **Java:** `methodName expectedResult` — `discoveryPortMatchesDriver()`, `gyroPacketHasCorrectTagAndLength()`

### What tests verify

| Layer | What it checks | Example |
|-------|---------------|---------|
| Wire constants | Port numbers, string literals, byte lengths match across components | `assert_eq!(TELEMETRY_PORT, 42071)` |
| Parser roundtrip | Encode → decode → compare fields | `build_gyro_packet()` → `parse_packet()` → assert all fields |
| State transitions | Flag changes on events (connect, timeout, reconnect) | `mark_driver_gone_if_stale()` clears `driver_connected` |
| Lifecycle | Start/stop/restart sequences | `FakeDiscovery.start()` → `onAckReceived()` → assert stopped |
| Contract cross-check | Android wire format matches bridge parser byte-for-byte | `CrossComponentContractTest.java` |

---

## Subagent Usage

Use the `task` tool to delegate work when:

### Always use subagents for:
- **Full codebase exploration** — `explore` agent to find files, patterns, or answer "where is X?"
- **Multi-file searches** — finding all usages of a function, port, or constant across all 3 components
- **Reading many files in parallel** — when you need to understand 5+ files before making a change
- **Verifying cross-component impact** — before changing a wire constant, search all 3 components for references

### Do NOT use subagents for:
- Reading 1-3 known files — just use `read` directly
- Simple edits — use `edit` or `write` directly
- Running a single command — use `bash` directly

### Subagent prompts should include:
1. The exact files to read (if known)
2. The exact information to return (file paths, line numbers, patterns)
3. Whether to make changes or just research

**Example good subagent prompt:**
```
Read every .rs file in cardboardplusplus-bridge/src/net/. For each file, report:
1. What it does (1 sentence)
2. The UDP/TCP port it uses and why
3. Key functions with line numbers
4. Error handling pattern used
Do NOT make any changes.
```

---

## Build & Test Verification

After any code change, run the relevant test command:

| Component | Test command | Build command |
|-----------|-------------|---------------|
| Bridge (Rust) | `cargo test --manifest-path cardboardplusplus-bridge/Cargo.toml` | `scripts/compile-bridge.ps1` |
| Driver (C++) | `scripts/compile-driver.ps1` | Same (tests are post-build events) |
| Android | `gradlew.bat testDebugUnitTest` (in `cardboardplusplus-android/`) | `scripts/compile-app.ps1` |
| Everything | `scripts/compile-all.ps1` | Builds all three |

### CI pipeline (matches GitHub Actions)
```
push/PR → [bridge test] ──┐
push/PR → [driver build] ─┼→ [installer] ─┐
push/PR → [android test] ─┘               └→ [release (on v* tags only)]
```

---

## Naming Conventions

### Rust (bridge)
- `snake_case` functions/variables, `PascalCase` structs/enums, `SCREAMING_SNAKE` constants
- Wire commands: `BRIDGE_HELLO`, `BRIDGE_ACK`, `CARDBOARD_CAP` (ALL CAPS)
- State functions: `note_*` (observe), `mark_*` (set flag), `check_*` (test condition)
- Test names: descriptive sentences — `stale_phone_clears_connected_flag`

### C++ (driver)
- `PascalCase` classes and functions: `HmdDriver`, `InitializeVideoEncoder()`
- `m_` prefix members: `m_encoderW`, `m_udpSocket`
- `k` prefix constants: `kDataPort`, `kBridgeHeartbeat`
- `test_` prefix test functions: `test_port_constants()`

### Java (android)
- `PascalCase` classes: `CameraController`, `VideoDecoder`
- `camelCase` methods: `startDiscovery()`, `feedFrame()`
- `UPPER_SNAKE` constants: `UDP_DISCOVERY_PORT`, `DISCOVERY_INTERVAL_MS`
- Test names: descriptive camelCase — `discoveryPortMatchesDriver()`

---

## Wire Protocol Details

### Port 42069 — H.264 Video (UDP, Driver → Phone + Bridge)
- Driver sends raw Annex-B to localhost:42069 for bridge preview (ffplay or embedded ffmpeg)
- Driver sends 4-byte big-endian length-prefixed H.264 to phone IP:42069
- Phone decodes via MediaCodec (hardware H.264)
- Bridge preview: ffmpeg `-f h264 -probesize 32 -i pipe:0 -vf scale=480:270 -f rawvideo -pix_fmt rgba`

### Port 42070 — Discovery & Control (UDP, bidirectional)
- Phone → Driver: `"CARDBOARD_DISCOVERY"` (every 500ms until ACK)
- Phone → Driver: `"CARDBOARD_CAP <W> <H>"` (no ACK, triggers resolution clamp)
- Driver → Phone: `"ACK"` (3 bytes)
- Bridge → Driver: `"BRIDGE_HELLO v1"` (every 500ms)
- Driver → Bridge: `"BRIDGE_ACK v1"` + `"BRIDGE_STATS fps=<n> bitrate=<kbps> frames=<n> drops=<n>"`
- Bridge → Driver: `"BRIDGE_CFG <fps> <bitrate_kbps> <encoder>"`
- Bridge → Driver: `"BRIDGE_PREVIEW 1"` or `"BRIDGE_PREVIEW 0"`

### Port 42071 — Telemetry (UDP, Phone → Bridge)
Binary packets:
| Tag | Type | Size | Payload |
|-----|------|------|---------|
| `0x10` | Gyro | 33B | u64 timestamp LE + 6×f32 LE (ang_vel + accel) |
| `0x11` | Hand | 15B | u64 timestamp LE + u8 hands + u8 landmarks + f32 confidence LE |
| `0x20` | Ping | 1B | bare tag byte |
| text | Hello | variable | `"CARDBOARD_PHONE_HELLO vN"` |

### Port 42072 — Camera (UDP, Phone → Bridge)
- Raw JPEG in single UDP datagram, max 60KB
- Bridge decodes JPEG → RGB → RGBA, optionally runs MediaPipe hand detection

### Port 42073 — MediaPipe (TCP, Bridge → Python)
- Bridge sends: `[u32 length LE][JPEG data]`
- Server replies: `[u8 num_hands]` then per hand: `[u8 handedness][f32 score LE][21 × Landmark]`
- Each `Landmark` = 3×f32 LE (x, y, z) = 12 bytes

### Port 8567 — REST API (TCP/HTTP, External → Bridge)
- `GET /health` → `{"ok":true,"app_version":"..."}`
- `GET /status` → StatusSnapshot JSON
- `GET /logs?n=50` → `{"logs":[...]}`
- `GET /preview` → preview state JSON
- `POST /preview` → toggle preview / open ffplay
- `POST /settings` → apply settings, echo back

---

## CardboardWire.h — DO NOT CHANGE

`driver_cardboardplusplus/include/CardboardWire.h` is the locked contract.
All wire constants are `static constexpr` in `namespace wire`:
- `kDataPort` (42069), `kDiscoveryPort` (42070)
- `kCardboardCap`, `kDiscoveryAck`, `kBridgeHeartbeat`, `kBridgeAck`, `kBridgeCfg`, `kBridgePreview`, `kBridgeStats`
- Each has a companion `kXxxLen` for prefix matching

The bridge and Android app have their own copies of these constants.
**Changing any value breaks all three components.** If you need to change a
wire constant, update ALL of:
1. `driver_cardboardplusplus/include/CardboardWire.h`
2. `cardboardplusplus-bridge/src/net/mod.rs` (port constants)
3. `cardboardplusplus-android/src/main/java/.../core/AppConstants.java`
4. All contract tests in all 3 components
5. This file (AGENTS.md)

---

## Common Pitfalls

1. **Don't add shared memory for IPC** — use UDP/TCP. Shared memory is platform-specific and adds synchronization complexity. The current UDP approach works across Windows/Android.

2. **Don't copy video frames into the bridge** — the driver sends to localhost:42069, the bridge reads from that socket directly. Adding a copy makes the bridge heavier for no benefit.

3. **Don't use TCP for real-time data** — head-of-line blocking causes visible freezes. A dropped UDP packet is replaced by the next one within milliseconds.

4. **Don't add mock frameworks** — the existing pattern of pure functions + fake inner classes + contract tests is simpler and faster. No Mockito, no mockall.

5. **Don't put unrelated files in the same Android folder** — each folder has one responsibility. If you're adding camera-related code, it goes in `camera/`.

6. **Don't skip contract tests** — when changing a wire format, add/update assertions in ALL components' test suites. The whole point is that components catch mismatches early.

7. **Don't remove the ring log cap** — `MAX_LOG_LINES = 200` in `app.rs` prevents memory growth. If you increase it, justify why.

8. **Don't change `$ErrorActionPreference = "Stop"` in scripts** — it's there for a reason. Always add `$LASTEXITCODE` checks after external commands (cargo, msbuild, gradlew, adb).

---

## Before You Edit: Checklist

- [ ] Did you search for all usages of the function/class you're changing?
- [ ] Does your change affect the wire protocol? If yes, update all 3 components
- [ ] Does your change affect test constants? Update contract tests
- [ ] Did you run the relevant test command?
- [ ] Did you verify the build still works?
- [ ] Is your change the smallest possible diff that solves the problem?
- [ ] Are you adding a new dependency? Is it truly necessary? (prefer stdlib)
- [ ] Are you adding a new file? Does it belong in an existing folder?
