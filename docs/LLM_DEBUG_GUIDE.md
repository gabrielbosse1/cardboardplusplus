# LLM Debug Guide

This guide tells an AI assistant how to enable, collect, and interpret debug
logs from all three components of Cardboard++ (Android app, bridge, SteamVR
driver) when troubleshooting issues like gyro drift, tracking loss, or video
stream problems.

## Quick Start

### 1. Enable debug logging

**Android app:** Open the settings menu in the app and toggle "Debug logging"
on. This persists across restarts via SharedPreferences.

**Bridge:** Start with the `--debug` flag or set the env var:
```
set CARDBOARD_DEBUG=1
cardboard-bridge.exe --headless
```
Or toggle at runtime via REST:
```
curl -X POST http://127.0.0.1:8567/debug -d "{\"enabled\":true}"
```

**Driver:** Set the env var before launching SteamVR:
```
set CARDBOARD_DEBUG=1
```
Then restart SteamVR. The driver checks the env var once at DLL load.

### 2. Collect logs

| Component | Where to find logs | How to tail |
|-----------|-------------------|-------------|
| **Android** | `adb logcat -s VideoDecoder TelemetrySender DiscoveryManager CameraController CameraStreamer VrActivity` | Real-time via ADB |
| **Bridge** | `GET /logs?n=200` on port 8567, or the Slint UI log panel | REST API or UI |
| **Driver** | `%ProgramFiles(x86)%\Steam\logs\vrserver.txt` | Open in editor, search for `[DEBUG]` |

### 3. What debug logging adds

When debug is OFF, only lifecycle events and errors are logged (connection
established, config applied, errors). When debug is ON, you get:

- **Telemetry:** Every sensor packet value (gyro, accel, mag, quaternion)
- **Discovery:** Every broadcast sent, every response received, every ACK
- **Video:** Every decoded frame count, FPS, codec capabilities
- **Camera:** Every frame size and count
- **Driver:** Every sensor packet, every encoded frame, every UDP send
- **Bridge:** Every telemetry packet type, every driver datagram

### 4. Log volume management

Debug logs are throttled to avoid overwhelming the log:
- **Android:** DebugLog only fires when the flag is set; warning/error always fire
- **Bridge:** `debug_log!` macro checks `debug_enabled()` before pushing to ring log
- **Driver:** `DebugLog` macro checks `CARDBOARD_DEBUG` env var; `DebugLogThrottle` fires every N calls
- **Ring log cap:** Bridge keeps max 200 lines; oldest are trimmed

### 5. Common debug scenarios

#### Gyro not working / tracking frozen
1. Enable debug on all three components
2. Check Android logcat for `TelemetrySender` — verify sensor values are changing
3. Check bridge logs for `[phone] rotation quat=` entries — verify quaternions change
4. Check driver `vrserver.txt` for `Rotation pkt` — verify quat values change
5. If phone sends but bridge doesn't receive: network/VPN issue
6. If bridge receives but driver doesn't: check bridge→driver forwarding on UDP 42074

#### Video stream not appearing
1. Check Android logcat for `Discovery successful` (ACK received)
2. Check bridge logs for `driver handshake established`
3. Check driver for `BRIDGE_HELLO` + `BRIDGE_ACK`
4. Check Android for `Decoded video FPS` — if 0, no frames arriving
5. Check driver for `[UDP] Sent framed packet` — if 0, encoder not producing

#### Phone not connecting
1. Check Android IP settings (VPN may change subnet)
2. Check discovery broadcasts in Android logcat: `Discovery sent to`
3. Check driver: `Discovery packet received from` — if none, broadcast not reaching driver
4. If using VPN, set PC IP in app settings manually

#### Hands invisible in SteamVR's own menus (dashboard, system UI, SteamVR Home)

**This is a Valve limitation, not a bug. Do not try to fix it in the driver.**

SteamVR's own UI runs as `openvr.component.vrcompositor`, and its action manifest
(`SteamVR/resources/config/vrcompositor_actions.json`) declares 44 actions: 34 boolean,
8 vector2, 1 pose, 1 vibration — and **no `skeleton` action at all**. There is no action to
bind bone data to, so no driver, ours or Valve's, can make that UI draw articulated fingers.
Real Index controllers show their controller model there for the same reason. Games differ
because each ships its own binding file; VRChat's binds `/skeleton/hand/*`, which is why hands
work in VRChat and nowhere else.

SteamVR Home (`tools/steamvr_environments/game/steamtours`) *does* declare
`/actions/global/in/HandSkeletonLeft|Right` and binds them, so the bones arrive — but its avatar
hands are pinned to `itemdefid = "v3_hands"` in `cfg/avatar_config.vcfg`, a three-finger glove
that Valve documents as not using skeletal input at all
([openvr#1606](https://github.com/ValveSoftware/openvr/issues/1606)). Finger curl there comes
from the trigger value.

The hand device publishes a pose and the skeleton only — it registers no button or
axis components — so SteamVR menu actions that bind `/input/trigger` resolve to a
dangling path and clicks silently never land. That is the same Valve limitation as
above, not a driver fault. There is deliberately **no `/input/system`** either:
SteamVR consumes that path as a complex button (click + double + held + long) and
rejects a plain boolean, and the OpenVR driver API has no complex-button constructor.

If the hands are missing entirely rather than unclickable:

1. Confirm the hands activated at all: `vrserver.txt` shows `Hand device spawned: CBPP_HAND_LEFT`
2. Look for `[Input] steam.client (knuckles) has no configured binding` — Steam's own client UI
   ships no binding file for `knuckles`, so the Steam overlay is unbound regardless

`Prop_ControllerType_String` and `Prop_RenderModelName_String` are the two high-leverage
properties: SteamVR selects every binding file from the first (its own menus, SteamVR Home, the
Steam client, each game), and selects the drawn model from the second. Two traps in the render
model name, both of which look like "the hand is broken":

- It must be **driver-qualified** — `{indexcontroller}/rendermodels/valve_controller_knu_1_0_left`,
  the same form Valve uses in `driver_vrlink.dll` (`{vrlink}/rendermodels/...`). Unqualified, SteamVR
  resolves it under *this* driver's resource root, finds nothing, and draws nothing at all.
- Omitting it entirely is worse still: SteamVR falls back to the global
  `resources/rendermodels/vr_controller_vive_1_5` and draws a Vive wand where the hand should be.

`oculus_touch` was tried as a "bare hand" controller type and reverted: it wins nothing visually,
and it inherits the same render-model fallback.

### 6. REST API debug endpoints

```
GET  /debug          → {"debug": true/false}
POST /debug          → {"enabled": true}  → toggle at runtime
GET  /logs?n=200     → newest 200 log lines
GET  /status         → full state snapshot
```

### 7. File locations

| Component | Log location |
|-----------|-------------|
| Android | `adb logcat` (runtime only) |
| Bridge | Ring log in memory + REST API |
| Driver | `%ProgramFiles(x86)%\Steam\logs\vrserver.txt` (SteamVR writes its logs to the folder named by the `log` key in `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`) |
| Driver (legacy) | Removed — old `C:\Temp\cbpp_pose.log` file logging was replaced by DebugLog |

### 8. Debug flag sources

| Component | How to enable | Env var / Setting |
|-----------|--------------|-------------------|
| Android | App settings toggle | `SharedPreferences("cardboard_plusplus_settings").getBoolean("debug_logging")` |
| Bridge | Env var or CLI flag | `CARDBOARD_DEBUG=1` or `--debug` |
| Driver | Env var | `CARDBOARD_DEBUG=1` (set before SteamVR launch) |
| Bridge REST | Runtime toggle | `POST /debug {"enabled":true}` |
