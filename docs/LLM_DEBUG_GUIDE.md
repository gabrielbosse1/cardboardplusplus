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
| **Driver** | `%LOCALAPPDATA%\OpenVR\vrserver.txt` | Open in editor, search for `[DEBUG]` |

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
| Driver | `%LOCALAPPDATA%\OpenVR\vrserver.txt` |
| Driver (legacy) | `C:\Temp\cbpp_pose.log` (REMOVED — now uses DebugLog) |

### 8. Debug flag sources

| Component | How to enable | Env var / Setting |
|-----------|--------------|-------------------|
| Android | App settings toggle | `SharedPreferences("cardboard_plusplus_settings").getBoolean("debug_logging")` |
| Bridge | Env var or CLI flag | `CARDBOARD_DEBUG=1` or `--debug` |
| Driver | Env var | `CARDBOARD_DEBUG=1` (set before SteamVR launch) |
| Bridge REST | Runtime toggle | `POST /debug {"enabled":true}` |
