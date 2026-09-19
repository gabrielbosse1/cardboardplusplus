# Cardboard++ Project Architecture & Vision

## The brief, once and for all

Cardboard++ is **one product with three parts**, and the **Bridge is the product** — the
pretty desktop hub that owns everything else. The SteamVR driver and the Android client
are not standalone apps; they are components of the Bridge. This document is the
authoritative description so that future agents (starting from a fresh chat) understand
the whole project before touching any code.

## The three parts

```
┌──────────────────────────────────────────────────────────────────────┐
│  BRIDGE  (desktop app, Rust + Slint)   = The Product                  │
│                                                                       │
│  ┌ Top bar: status, framerate, connected/not, current stream state ┐ │
│  └──────────────────────────────────────────────────────────────────┘ │
│  ┌ Side bar:                                                        ┐ │
│  │  · Stream  — resolution, bitrate, speed/FPS, encoder, start/stop │ │
│  │  · Camera  — phone camera passthrough, MediaPipe hand tracking    │ │
│  │  · General — driver installer, client/apk installer, settings    │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                                                       │
│  OWNS: config, settings, installers, monitoring, debugging, the UI.   │
└──────────────────────────────────────────────────────────────────────┘
                          │ shared memory (bridge-shm) + config
              ┌───────────┴───────────┐
              ▼                       ▼
┌──────────────────────┐   ┌──────────────────────────────┐
│  STEAMVR DRIVER      │   │  ANDROID CLIENT              │
│  (C++, in-process)   │   │  (cardboardplusplus-android) │
│  · creates HMD       │   │  · Cardboard SDK renderer    │
│  · encodes H264      │   │  · decodes H264 (MediaCodec) │
│  · sends UDP to phone│   │  · camera preview            │
│  · hand-tracking axis│   │  · camera JPEG → Bridge      │
│    relay → SteamVR   │   │    (MediaPipe runs there)    │
└──────────────────────┘   └──────────────────────────────┘
```

## What the Bridge does (and MUST do)

### 1. It is the pretty desktop app — the whole interface

- **Top bar** shows: connection status ("Connected/Idle/Error"), live framerate,
  whether the driver is up, whether a client is connected.
- **Side bar** has sections:
  - **Stream**: edit resolution, bitrate, speed/FPS, encoder selection, start/stop.
    These settings are pushed down to the driver (and mirrored for the client).
  - **Camera**: phone camera passthrough + hand tracking. The phone streams
    camera JPEGs to the Bridge, which runs the MediaPipe hand model in a Python
    sidecar (`bridge/crates/cardboard-bridge/mediapipe_server.py`, TCP 42073);
    on-phone inference is a future option, not the current path. The hand data is sent back to the
    SteamVR driver, which feeds it to SteamVR as bone/controller input. This means
    hands are exposed to SteamVR apps like real tracked objects.
  - **General**: automatic installers. The Bridge ships with the compiled driver
    and the compiled Android APK(s); from here the user can install/update the
    SteamVR driver and install the APK on the phone over ADB. It is a one-click
    installer/updater for the whole product.

### 2. It owns all configuration and settings

- All persistent settings live in the Bridge (not the driver, not the app).
- The driver reads config from the Bridge; the Bridge can edit it live and the
  driver applies it (via shared memory / a settings pipe).

### 3. It is the debugging/monitoring surface

- Because the Bridge is the hub, debugging a feature (e.g. "is the stream
  connected? is hand tracking working?") is done from one place. A stream-only
  fix that leaves hand tracking broken is a debugging nightmare — therefore:

> **The stream must NOT be separable from the rest of the product.** Functionality
> is only "done" when the whole pipeline works from the Bridge. Turn a feature on
> only from the Bridge; the Bridge is the single switch and the single readout.

## Video transport (decided)

- **Keep UDP for the video stream**: the driver encodes H264 in `VideoEncoder.cpp`
  and sends it over UDP (`m_udpSocket`) to the phone (port 42069, 4-byte
  big-endian length-prefixed). This stays as-is.
- The **Bridge never sits in the video path** — video goes driver → phone
  directly. The driver additionally sends a raw Annex-B copy to localhost:42069,
  which the Bridge binds and pipes to ffmpeg purely for **preview display**
  (decoded to RGBA in a separate process; no frame copies in Rust).
- The Bridge is the **control + telemetry plane over UDP**:
  - Bridge ↔ driver discovery/control (UDP 42070: `BRIDGE_HELLO`/`BRIDGE_ACK`/
    `BRIDGE_STATS`/`BRIDGE_CFG`/`BRIDGE_PREVIEW`)
  - Phone → Bridge telemetry (UDP 42071) and camera JPEGs (UDP 42072),
    forwarded Bridge → driver as sensors (UDP 42074)
  - MediaPipe sidecar over TCP 42073, REST API on 8567
- A **local shared-memory channel** (`bridge-shm`, `protocol.rs` mirrored by
  the driver's `BridgeProtocol.h`) additionally carries driver → Bridge
  status/telemetry and settings. It is NOT for video frames and no new
  SHM channels should be added — new data paths go over UDP/TCP.
- The phone app decodes the UDP H264 with MediaCodec and renders. It streams
  camera JPEGs to the Bridge, which is the hand-tracking compute node
  (MediaPipe sidecar, TCP 42073).

## Hand tracking (the reason the whole thing must ship together)

- Runs on the **Bridge** using the phone's camera stream: the phone sends JPEG
  frames (UDP 42072), the Bridge forwards them to `mediapipe_server.py` (TCP
  42073) and gets back 21-landmark hands, which go to the SteamVR driver.
- Running the model **on the phone** is a future option (the phone already sends
  `0x11` hand-presence hints over telemetry); today the phone does no inference —
  its `tracking/` folder is empty.
- The driver exposes the hands to SteamVR as tracked skeleton/controller input, so
  VR apps see your hands.
- **Integration rule:** a stream that works but has no hand tracking is NOT a
  working feature. Both always activate/deactivate together, from the Bridge.

## Installers (General section)

- The Bridge holds the built artifacts:
  - SteamVR driver DLL (and ffmpeg/compiled deps)
  - Android APK(s)
- From **General** in the UI the user can:
  - Install/update the SteamVR driver into `SteamVR\drivers\cardboardplusplus\`
    (backup existing DLL first).
  - Install/update the APK on a connected phone (`adb install`, discovery already
    exists on both sides).
- ADB-over-Tailscale is a maintainer remote-test workflow (needs your own
  Tailscale setup; no guide yet): the phone connects via its Tailscale IP and
  `adb connect <ip>:<port>`; the PC is the host.

## Current implementation status (so agents don't re-derive it)

### bridge/ (Rust workspace)
- `crates/bridge-shm` — the shared-memory transport. `protocol.rs` defines the
  SHM layout (RegionHeader + slot ring, latest-wins). Authoritative copy for
  the SHM layout (the UDP wire contract lives in `CardboardWire.h` instead).
- `crates/bridge-core` — consumer facade (`shm.rs` + future `glue`/`d3d11`).
- `crates/bridge-ui` — Slint desktop UI (status, stream/camera settings,
  installers, diagnostics; see README "Current state" for what works today).
  This is where the real product UI goes.

### driver_cardboardplusplus/ (C++, MSVC)
- SteamVR direct-mode driver: HmdDriver, VideoEncoder (H264 via ffmpeg), UDP,
  discovery (`CARDBOARD_CAP`, IP switch), controller/device factory.
- **Bridge producer added** (`BridgeServer.h/.cpp`): creates the named shared region
  `Local\cardboard_pp_bridge`, publishes `FrameSubmitted`/telemetry. Layout header
  `include/BridgeProtocol.h` is the C mirror of the Rust protocol (keep in sync).
- Wire-layout sizes were verified: payloads use NATURAL alignment (not packed).

### cardboardplusplus-android/ (Java + JNI/C++ + Cardboard SDK)
- Renders VR, decodes H264 (MediaCodec), camera passthrough (Camera2), UDP client,
  discovery sender, decoder-cap reporting, PC-IP override setting.

## Non-negotiables for future agents

1. **Two locked contracts, each with one source of truth:**
   `driver_cardboardplusplus/include/CardboardWire.h` for the UDP wire
   (ports, strings, telemetry tags); `bridge/crates/bridge-shm/src/protocol.rs`
   for the SHM layout, with `driver_cardboardplusplus/include/BridgeProtocol.h`
   mirroring it byte-for-byte (natural alignment, verified sizes).
   Touch both sides of whichever contract you change.
2. **The Bridge is the product.** No driver-only, app-only, or ad-hoc feature
   mutations that bypass the Bridge. Settings, installs, monitoring, and the
   on/off switch live in the Bridge.
3. **Stream and hand tracking ship together.** A feature is done only when the
   whole chain works and is observable from the Bridge.
4. **Video stays on UDP.** Shared memory is for control/telemetry, not encoded
   frames (today). Revaluating "bridge carries video" later is allowed, but only
   as a deliberate architecture change, not a drift.
5. Keep the existing remote test workflow: ADB over Tailscale; SteamVR drivers
   install with DLL backup; `bridge-ui` verifies the region via WIN32 probes.

## Open questions / decisions to revisit
- Whether shared memory should later carry encoded frames (vs UDP only).
- Hand-tracking transport details (same UDP channel vs dedicated).
- Encoder/settings push-down protocol (driver ↔ Bridge direction).