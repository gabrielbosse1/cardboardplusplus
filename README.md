# Cardboard++
## Bringing Google Cardboard closer to a Meta Quest

### This project is not ready for use

There is no end-user installer — you must build and install everything manually. Hand tracking is not functional yet. This is a development-only project at this stage; expect rough edges and incomplete features.

![License](https://img.shields.io/github/license/gabrielbosse1/cardboardplusplus)
![Stars](https://img.shields.io/github/stars/gabrielbosse1/cardboardplusplus)
![Last Commit](https://img.shields.io/github/last-commit/gabrielbosse1/cardboardplusplus)

The project goal is: bring features normally exclusive to expensive VR headsets (like the Meta Quest) to a simple Google Cardboard. Features include hand tracking, 6DoF, SteamVR compatibility, and using Xbox controllers as virtual VR controllers.

> **A note on code quality:** Parts of the codebase are rough or contain redundant work; cleanup is in progress. Contributions are welcome.

---

## Current state

The project is functional but needs cleanup. The SteamVR driver captures frames, encodes to H.264, and streams to the Android app over UDP. The Android app decodes and renders in VR via Google Cardboard. The Bridge desktop app (Rust) is the control plane: settings, installers, monitoring — it never touches the video stream.

### What works

**Bridge (`bridge/crates/cardboard-bridge/` — the product, Rust + Slint)**
- Slint desktop UI: status, stream/camera settings, driver + APK installers, diagnostics log (200-line ring buffer)
- REST API on `127.0.0.1:8567`: `GET /health /status /logs?n= /preview /debug`, `POST /preview /settings /debug`
- Driver link (UDP 42070): `BRIDGE_HELLO` heartbeat every 500ms, handles `BRIDGE_ACK` + `BRIDGE_STATS`, sends `BRIDGE_CFG` / `BRIDGE_PREVIEW` / `CARDBOARD_CAP`
- Phone telemetry ingest (UDP 42071): gyro `0x10` (45B), hand `0x11` (15B), rotation quat `0x12` (25B, the real head-tracking path), net-stats `0x13` (21B), ping `0x20`, `CARDBOARD_PHONE_HELLO`; forwards sensors to driver on UDP 42074
- Camera ingest (UDP 42072 JPEG) → MediaPipe over TCP 42073 (`mediapipe_server.py`, 21-landmark hand skeleton overlay; model file `models/hand_landmarker.task` is vendored)
- Preview: binds UDP 42069, pipes to ffmpeg → RGBA, optional ffplay popup
- Installed-artifact serving: driver DLL + APK install/update from the UI (ADB supported) (Still in development, going to ship in the finished app)

**SteamVR driver (`driver_cardboardplusplus/`, C++ DLL)**
- HMD registration, Vive-style controller expose, D3D11 direct-mode present path
- H.264 encoding (FFmpeg: AMF/NVENC/QSV detection, libx264 fallback), GPU eye-composite shaders + readback
- UDP transport: length-prefixed H.264 to phone:42069 + raw Annex-B copy to localhost:42069 for Bridge preview
- Discovery (UDP 42070): answers `CARDBOARD_DISCOVERY` with `ACK`, honors `CARDBOARD_CAP` resolution clamp, answers `BRIDGE_HELLO` with `BRIDGE_ACK` + `BRIDGE_STATS`
- Sensor intake (UDP 42074) from Bridge-forwarded phone telemetry
- Shared-memory producer (`Local\cardboard_pp_bridge`): status/telemetry ring, settings channel (`BridgeProtocol.h` mirrors `protocol.rs`)

**Android app (`cardboardplusplus-android/`, Java + JNI/C++)**
- VR rendering (`render/`): Cardboard lens distortion, zero-copy `SurfaceTexture` → OpenGL
- Video (`video/`): UDP reassembly → MediaCodec hardware decode, stall watchdog re-triggers discovery, decoder-cap reporting (`CARDBOARD_CAP W H`)
- Discovery (`discovery/`): broadcasts `CARDBOARD_DISCOVERY` every 500ms until `ACK`, then sends cap
- Telemetry (`telemetry/`): gyro/accel/mag `0x10` + game-rotation-vector quaternion `0x12` + hello → UDP 42071
- Camera (`camera/` + `streaming/`): Camera2 → 256x192 JPEG q38 @30fps + u16 seq header → UDP 42072
- Settings UI (`settings/`): resolution/FPS/bitrate/codec persisted in SharedPreferences; codec fallback H264 → HEVC/AV1 probe (`codec/`)

### What I'm working on now

- **Fixing redundant work** — there are places where the same data gets converted multiple times (AVCC→Annex B→length-prefix→Annex B). Cleaning this up.
- **Moving CPU work to GPU** — the BGRA→NV12 color conversion currently happens on the CPU via FFmpeg's sws_scale. Moving this to a compute shader.
- **Understanding and cleaning the codebase** — removing dead code, fixing misleading flags, aligning resolution values.
- **Linux support** — after the core fixes are done, making the driver work on Linux (replacing D3D11 with Vulkan on the linux version).

### Not yet ported / incomplete

- Hand tracking on the phone (MediaPipe currently runs on the Bridge; porting inference to the app is future work)
- 6DoF tracking (IMU-based, needs porting to new app)
- External gamepad as VR controllers

---

## Project structure

```
cardboardplusplus/
├── bridge/                             # Rust — the product (control plane, never touches video)
│   └── crates/
│       ├── cardboard-bridge/           # Real app: Slint UI, REST :8567, UDP workers, MediaPipe TCP client
│       │   └── src/                    # main.rs, app.rs, core.rs, server.rs (+handlers), net/, hand_overlay.rs
│       ├── bridge-shm/                 # SHM transport; protocol.rs = wire-layout source of truth
 │       ├── bridge-core/                # SHM consumer facade (shm.rs) + default paths (paths.rs)
│       └── bridge-ui/                  # New Slint product UI (status/stream/camera/install/diagnostics)
├── driver_cardboardplusplus/           # SteamVR driver (C++ DLL)
│   ├── src/                            # HmdDriver, VideoEncoder*, EncodingThread, UdpTransport,
│   │                                   # Discovery, BridgeServer (SHM producer), ControllerDriver, DeviceProvider
│   ├── include/                        # CardboardWire.h (locked UDP contract), BridgeProtocol.h (SHM mirror)
│   └── tests/                          # Driver tests (mock SteamVR, mock bridge)
├── cardboardplusplus-android/          # Android VR app (Java + JNI/C++)
│   └── src/main/java/com/google/cardboard/
│       ├── camera/ codec/ discovery/ network/ permissions/ render/
│       ├── settings/ streaming/ telemetry/ ui/ video/   # one folder per responsibility
│       ├── VrActivity.java             # Entry point (root package)
│       └── NativeBridge.java           # JNI interface (root package)
├── scripts/                            # compile-bridge/driver/app/all.ps1, install-driver/app.ps1
├── docs/                               # PROJECT_VISION.md (architecture), LLM_DEBUG_GUIDE.md
├── sdk/ third_party/ proto/            # Cardboard SDK, Unity XR headers, device-params protobuf
└── LICENSE                             # GPL v3
```

Wire protocol (locked — `CardboardWire.h` ↔ `bridge/.../net/mod.rs` ↔ `AppConstants.java`):

| Port  | Protocol | Direction |
|-------|----------|-----------|
| 42069 | UDP H.264 | Driver → Phone + Bridge (preview) |
| 42070 | UDP text | Phone ↔ Driver, Bridge ↔ Driver (discovery/control) |
| 42071 | UDP binary | Phone → Bridge (telemetry) |
| 42072 | UDP JPEG | Phone → Bridge (camera) |
| 42073 | TCP binary | Bridge → Python MediaPipe |
| 42074 | UDP binary | Bridge → Driver (sensor forward) |
| 8567  | HTTP REST | External → Bridge |

---

## How it works

### Bridge (`bridge/`) — control + telemetry plane, never video

1. Pushes settings (`BRIDGE_CFG`, `CARDBOARD_CAP`, `BRIDGE_PREVIEW`) to the driver over UDP 42070
2. Ingests phone telemetry (UDP 42071) and camera JPEGs (UDP 42072), forwards sensors to the driver (UDP 42074)
3. Sends camera frames to Python MediaPipe over TCP 42073, overlays the 21-landmark hand skeleton
4. Reads preview H.264 from localhost UDP 42069 via ffmpeg (separate process, no frame copies in Rust)
5. Exposes everything over REST (`:8567`) and the Slint UI; installs driver DLL + APK from the General section

### SteamVR driver (`driver_cardboardplusplus/`)

1. SteamVR renders into shared D3D11 textures via `IVRDriverDirectModeComponent`
2. `VideoEncoder` composites both eyes into a side-by-side frame on the GPU
3. GPU→CPU readback, BGRA→NV12 conversion, FFmpeg H.264 encoding
4. Encoded packets sent over UDP to the phone

### Android app (`cardboardplusplus-android/`)

1. `VideoReceiver` reassembles UDP datagrams into complete frames
2. `VideoDecoder` feeds Annex B H.264 to MediaCodec hardware decoder
3. Decoded frames output to a `SurfaceTexture` (zero-copy to OpenGL)
4. `VrRenderer` renders the video with Cardboard lens distortion per-eye

---

## Building

All components build/test from `scripts/` (each checks `$LASTEXITCODE`):

```powershell
scripts/compile-all.ps1      # everything
scripts/compile-bridge.ps1   # cargo build (bridge)
scripts/compile-driver.ps1   # MSVC Release|x64 (driver, tests are post-build events)
scripts/compile-app.ps1      # gradle assemble (APK)
scripts/install-driver.ps1   # install driver into SteamVR (backs up existing DLL)
scripts/install-app.ps1      # adb install -r APK on phone
```

Tests (mocked collaborators, no mock frameworks — pure functions + fakes + contract tests):

```powershell
cargo test --manifest-path bridge/Cargo.toml   # bridge unit + integration (mock driver + mock phone)
# android: run `gradlew.bat testDebugUnitTest` inside cardboardplusplus-android/
```

### SteamVR driver (manual)

Open `driver_cardboardplusplus/driver_cardboardplusplus.sln` in Visual Studio. Build `Release|x64`.

Requires:
- Visual Studio 2022 with C++ workload + Windows SDK (for MSBuild, found via vswhere)
- FFmpeg (bundled in `driver_cardboardplusplus/lib/ffmpeg/` — `lib` + `include` only; `lib/ffmpeg/doc/` is git-ignored, not vendored)
- OpenVR SDK (bundled in `include/` and `lib/`)

### Android app (manual)

Open the repo root in Android Studio — `settings.gradle` maps the `:app` module to `cardboardplusplus-android/`, so the root project is what loads it. Build and install on your phone. Copy `local.properties.example` to `local.properties` and set `sdk.dir` to your Android SDK path.

Requires:
- JDK 17 (matches CI), Android SDK (API 24+, build-tools + platform + licenses accepted)
- NDK (for native C++ code)
- Google Cardboard SDK (bundled)
- Rust stable toolchain (edition 2021, for the bridge)
- Python 3 with `mediapipe`, `opencv-python`, `numpy` (for `mediapipe_server.py` on TCP 42073)
- ffmpeg/ffplay on PATH (for bridge preview decode)

---

## Contributing

This is a work in progress. **All contributions are welcome.**
See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

[GNU General Public License v3.0](LICENSE)

This project links against FFmpeg (libavcodec, libavutil, libswscale), which includes libx264. Since libx264 is GPL v2+, the combined work is licensed under GPL v3.
