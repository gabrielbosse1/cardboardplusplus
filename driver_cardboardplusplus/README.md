# CardboardPlusPlus SteamVR Driver

Virtual HMD driver that captures SteamVR compositor frames, encodes to H264, and sends via UDP.

## Build

Open `driver_cardboardplusplus.vcxproj` in Visual Studio and build Release|x64.

Requires:
- FFmpeg (libavcodec, libavformat, libavutil, swscale) - included in `lib/ffmpeg/`
- OpenVR SDK - included in `include/` and `lib/`

## Install

Copy to `<SteamVR>/drivers/cardboardplusplus/bin/win64/`:
- `x64/Release/driver_cardboardplusplus.dll`
- All FFmpeg DLLs from `lib/ffmpeg/` (avcodec-62.dll, avformat-62.dll, avutil-60.dll, swscale-9.dll, etc.)

## Test

Start SteamVR, then view the live H264 stream with ffplay:

```
ffplay -f h264 -an udp://127.0.0.1:42069
```

## Details

- Output: H264 SBS (side-by-side), 2880x1620, left eye on left, right eye on right
- Port: UDP 42069 (no handshake, just sends)
- Encoder: libx264, fast preset, zerolatency, 20Mbps
- Texture format: SteamVR's requested format (typically R10G10B10A2)
