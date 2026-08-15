# Cardboard++ SteamVR Driver

Virtual HMD driver that captures SteamVR compositor frames, encodes to H.264, and streams to an Android phone over UDP.

## How it works

1. Registers as a virtual HMD with SteamVR via `IVRDriverDirectModeComponent`
2. SteamVR renders into shared D3D11 textures (one per eye)
3. `VideoEncoder` composites both eyes into a side-by-side frame using a GPU shader pass
4. GPU→CPU readback, BGRA→NV12 conversion, FFmpeg H.264 encoding
5. Encoded packets sent over UDP (port 42069) to the phone

The phone announces itself via UDP broadcast (port 42070). On discovery, the driver switches the data target to the phone's IP. The phone also sends its hardware decoder cap (max resolution) so the driver can clamp the encoder to a compatible size.

## Build

Open `driver_cardboardplusplus.sln` in Visual Studio and build **Release|x64**.

Requires:
- FFmpeg (libavcodec, libavformat, libavutil, swscale) — included in `lib/ffmpeg/`
- OpenVR SDK — included in `include/` and `lib/`

## Install

Copy to `<SteamVR>/drivers/cardboardplusplus/bin/win64/`:

- `driver_cardboardplusplus.dll` (from build output)
- All FFmpeg DLLs from `lib/ffmpeg/` (`avcodec-*.dll`, `avformat-*.dll`, `avutil-*.dll`, `swscale-*.dll`, etc.)

## Test

Start SteamVR, then view the live H.264 stream with ffplay:

```
ffplay -f h264 -an udp://127.0.0.1:42069
```

## Configuration

Default encoder settings (can be changed in code):

| Setting | Value |
|---------|-------|
| Resolution | 2880x1620 SBS (1440x1620 per eye) |
| FPS | 60 |
| Bitrate | 20 Mbps |
| Encoder | Auto-detect: AMF > NVENC > QSV > libx264 |
| GOP size | 10 |
| B-frames | 0 (zero-latency) |
| Profile | Baseline |

## Encoder selection

The driver tries hardware encoders in order:
1. `h264_amf` (AMD GPU)
2. `h264_nvenc` (NVIDIA GPU)
3. `h264_qsv` (Intel GPU)
4. `libx264` (software fallback, ultrafast/zerolatency)

The selected encoder is logged on startup.

## Network protocol

- **Port 42069 (UDP):** Encoded H.264 frames. Each frame is prefixed with a 4-byte big-endian length. Large frames are fragmented into 60KB chunks.
- **Port 42070 (UDP):** Discovery. Phone broadcasts `CARDBOARD_DISCOVERY`, driver replies with `ACK`. Phone can also send `CARDBOARD_CAP <w> <h>` to negotiate resolution.

## Known issues

See `CODEBASE_ISSUES.md` in the project root for a detailed list of architectural problems and redundant work being addressed.

- Encoding blocks SteamVR's compositor thread (should use PostPresent or separate thread)
- BGRA→NV12 conversion happens on CPU instead of GPU
- Fake triple-buffering (same texture handle for all 3 swap slots)
- Manual keyframe NAL fixup is fragile and duplicates BSF work
- `m_encoderUseGpu` flag does nothing (dead code)

## License

[GNU General Public License v3.0](LICENSE)

This driver links against FFmpeg, which includes libx264 (GPL v2+). The combined work is licensed under GPL v3.
