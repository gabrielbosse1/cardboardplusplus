# Camera Pipeline Rebuild Plan — 30 fps MediaPipe Feed

Goal: 30 fps phone→bridge camera feed, just enough resolution for MediaPipe,
fast on most phones + PCs, no new native deps. UI displays at true speed.

## 1. What MediaPipe actually needs (measured, not guessed)

- Official HandLandmarker bundle input shape: **192×192, 224×224**
  (`ai.google.dev/edge/mediapipe/solutions/vision/hand_landmarker` — Models table).
- Internal landmark tensor: **224×224**
  (`mediapipe/modules/hand_landmark/hand_landmark_gpu.pbtxt`: `output_tensor 224x224`,
  `input_image 224x224`).
- BlazePalm detector coordinate scale: **256**
  (`blazepalm.py`: `x_scale = y_scale = 256.0`).
- API accepts any size (`HandLandmarker.py`: "image can be of any size") — it
  **downscales internally**. Anything above ~256 px long edge is pure waste:
  more WiFi bytes, slower `cv2.imdecode`, slower palm-resize, zero accuracy gain.
- Python side already uses `RunningMode.VIDEO` (`mediapipe_server.py:45`), so
  palm-detection runs infrequently + tracking reuses landmarks. Keep it.

**Decision: wire format = 256×192 (4:3, 49 kpx).**
36% fewer pixels than current 320×240 (77 kpx). 256 matches BlazePalm scale;
192 keeps 4:3 camera aspect (no letterbox distortion for palm boxes).
Shrink cost on phone (one nearest-neighbor YUV pass, ~50 kpx) is trivial vs.
the current double-JPEG-encode it replaces. **Shrink is worth it — do it.**

## 2. Transport decision (why JPEG-over-UDP stays)

| Option | Verdict |
|---|---|
| Raw RGB/YUV over UDP | Reject: 256×192×3 = 147 KB > 64 KB datagram limit → fragmentation + reassembly + worse loss behavior. |
| H.264 via MediaCodec | Reject: encoder latency, SPS/PPS + keyframe logic, new failure modes — overkill for 256 px. |
| TCP | Reject: head-of-line blocking on a realtime stream (AGENTS.md rule). |
| **Optimized JPEG-over-UDP, one datagram per frame, latest-wins** | **Keep.** `YuvImage.compressToJpeg` is native-fast, universal on every phone; UDP drop = next frame in 33 ms. |

New wire (port 42072 unchanged): `[u16 seq BE][JPEG bytes]`, max 60 KB
unchanged. `seq` enables drop counting + latest-wins drain. Resolution fixed
by contract (256×192), no per-frame header needed.

Bandwidth at q35–40, 256×192: ~8–15 KB/frame → 30 fps ≈ **250–450 KB/s**.
Fits any WiFi; stays under 60 KB guard with margin (current q50 320×240
silently drops at `CameraStreamer.java:113`).

## 3. New phone pipeline (replaces double-encode)

Current waste (`CameraStreamer.java:94-110`): NV21 → JPEG encode → Bitmap
decode → Canvas scale → JPEG re-encode, all on camera thread, 15 fps cap
(`FRAME_INTERVAL_MS = 1000/15`), silent drop >60 KB.

New `CameraStreamer.onFrame`, same thread, no new threads:
1. Throttle to **33 ms (30 fps)**.
2. **Downscale YUV_420_888 → 256×192 first** (stride-aware nearest-neighbor,
   single pass, Y + interleaved VU). No Bitmap, no Canvas, no second encode.
3. **Single** `YuvImage.compressToJpeg(..., quality 35-40)`.
4. Prepend `seq++`, `socket.send`. Keep `>60000` guard but **count drops**
   (log every 60) instead of silent return.
5. Request **640×480** from Camera2 (cheap, universal) — downsample does the rest.

`CameraController` dual-use split: keep texture/passthrough path untouched;
remove only `FrameCallback`/`ImageReader`-as-streamer-target coupling if it
blocks the 640×480 request (detail at implementation time).

## 4. New bridge pipeline (decode off the hot path)

Current stall (`net/camera.rs:41-119`): blocking `recv` → pure-Rust
`jpeg-decoder` → **blocking TCP `detect()` with no timeout**
(`net/mediapipe.rs:95-146`) → overlay → store, all serial.
FPS = 1 / (decode + TCP roundtrip). One slow MediaPipe call stalls everything.
UI polls at 250 ms (`main.rs:97`) → 4 Hz display max.

New shape, 3 threads, stdlib only (`mpsc` + `Mutex`, no new deps):
1. **Recv thread**: socket non-blocking, drain to **latest seq**, push raw JPEG
   bytes into a bounded channel (depth 2, latest-wins). **No JPEG decode here.**
2. **MediaPipe worker**: `try_recv` latest only (skip-if-busy — never queue);
   blocking `detect()` with **read/write timeouts** (2 s); publishes landmarks
   to shared state. Slow Python can no longer stall recv or preview.
3. **Preview worker**: decodes **every 3rd frame (~10 fps)** to RGBA for UI
   only. Swap `jpeg-decoder` → `zune-jpeg` (pure Rust, drop-in, ~2-3× faster,
   no native lib). Full-frame decode for overlay stays at preview rate.
4. **UI poll 250 ms → 33–50 ms** (`main.rs:97`) so the view shows true speed.
   `camera-frame`/`preview-frame` both benefit.

`mediapipe_server.py`: keep `VIDEO` mode + TCP 42073 landmark protocol
**unchanged** (no wire break downstream). Two fixes only: monotonic timestamp
(`frame_idx * 33` instead of wall-clock `time.time()`, VIDEO mode requires
monotonic) and keep `listen(1)` + 1 s timeout as-is.

## 5. What gets removed (dead code)

Delete whole files:
- `bridge/.../src/net/camera.rs` → replaced by new `net/camera_fast.rs`
  (recv + workers). Old decode-inline + blocking-detect shape goes.
- `bridge/.../src/hand_overlay.rs` → fold 1 function into preview worker
  (or keep file if smaller diff — decide at implementation; default: fold).
- `CameraStreamer.java` double-encode block (`imageToNv21` stays, remove
  `BitmapFactory.decode` → Canvas → recompress lines 98-110, `scaleBuffer`
  field) + `CameraStreamerTest.java` expectations (q50/320×240/15fps).
- `jpeg-decoder` dep line (`Cargo.toml:12`) → `zune-jpeg`.

Delete blocks (partial files):
- `app.rs`: `note_camera_frame`/`check_camera_liveness` rework to seq-based
  latest-wins + fps counter (add `camera_fps`, currently dead —
  `ui/app.slint:61 camera-fps` declared but never set).
- `core.rs`: `spawn_mediapipe_server` keep, rewire spawn call to new module.
- `main.rs:130-132`: keep setters, faster timer.
- Tests: `mock_phone.rs` JPEG-send test update to new header + size;
  `CrossComponentContractTest`/`WireContractTest` camera consts update
  (port same, resolution + quality + seq header new).

Keep untouched: `CameraController` passthrough leg, `VrRenderer`,
`NativeBridge`, `AndroidManifest` CAMERA permission, telemetry `0x11` hand
hints over 42071, all 42070/42069/42073 contracts, `AGENTS.md` transport rules.

## 6. Contract changes (all three components)

1. `AppConstants.java`: `TARGET 320×240/q50/15fps` → `256×192/q35-40/30fps`
   + `SEQ_HEADER_LEN = 2`.
2. `net/mod.rs` + `CardboardWire.h` + `AppConstants`: resolution/quality/fps
   comments (port 42072 unchanged, no value break).
3. All camera contract tests updated to new consts.
4. Docs prose: `AGENTS.md` 42072 row, `PROJECT_VISION.md` camera ownership,
   `README.md` table, `LLM_DEBUG_GUIDE.md` tags.

## 7. Verification

1. `cargo test --manifest-path bridge/crates/cardboard-bridge/Cargo.toml`
2. Android `testDebugUnitTest` (streamer rate/size/header tests).
3. Live: bridge log `camera frame #` rate ≈ 30/s, `camera_fps` pill ≈ 30,
   MediaPipe hands overlay <100 ms behind reality, no silent drops.
4. Fallback: if 30 fps unstable on a device, single knob `FRAME_INTERVAL_MS`
   33→66 (phone) + preview decimation 3→6 (bridge). No redesign.

## 8. Step order (one thing at a time, build + test each)

1. Phone: downscale-first + single encode + seq header + 30 fps cap.
2. Bridge: non-blocking recv + channel + skip-if-busy worker + timeouts.
3. Bridge: preview decimation + `zune-jpeg` + 33 ms UI poll.
4. Server: monotonic timestamp fix.
5. Contract tests + docs prose.
6. Live triple-verify (bridge fps pill, overlay latency, drop counter).
