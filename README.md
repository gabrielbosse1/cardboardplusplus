# Cardboard++
## Bringing Google Cardboard closer to a Meta Quest

![License](https://img.shields.io/github/license/gabrielbosse1/cardboardplusplus)
![Stars](https://img.shields.io/github/stars/gabrielbosse1/cardboardplusplus)
![Last Commit](https://img.shields.io/github/last-commit/gabrielbosse1/cardboardplusplus)

The project goal is: bring features normally exclusive to expensive VR headsets (like the Meta Quest) to a simple Google Cardboard. Features include hand tracking, 6DoF, SteamVR compatibility, and using Xbox controllers as virtual VR controllers.

> **A note on code quality:** This project was largely vibecoded, I left an LLM working on the encoder and Android app while I focused on hand-coding another project. I'm aware that some parts of the codebase are rough, have redundant work, or do things the wrong way. I'm actively fixing these issues. If you're here to help, you're welcome, and we really need some help.

---

## Current state

The project is functional but needs cleanup. The SteamVR driver captures frames, encodes to H.264, and streams to the Android app over UDP. The Android app decodes and renders in VR via Google Cardboard.

### What works

- SteamVR driver (HMD registration, controller, video encoding/streaming)
- Android VR app (Camera passthrough, video decoding via MediaCodec, Cardboard lens distortion)
- Dynamic resolution negotiation (phone tells PC its decoder cap)
- Hardware encoder detection (AMF, NVENC, QSV, libx264 fallback)

### What I'm working on now

- **Fixing redundant work** — there are places where the same data gets converted multiple times (AVCC→Annex B→length-prefix→Annex B). Cleaning this up.
- **Moving CPU work to GPU** — the BGRA→NV12 color conversion currently happens on the CPU via FFmpeg's sws_scale (AI didn't know what is was doing). Moving this to a compute shader.
- **Understanding and cleaning the codebase** — removing dead code, fixing misleading flags, aligning resolution values (like the use GPU encoding that is doing absolutely nothing if i set to false, and does not need to exist at all).
- **Linux support** — after the core fixes are done, making the driver work on Linux (replacing D3D11 with Vulkan on the linux version).

### Not yet ported / incomplete

- Hand tracking (MediaPipe, needs porting to new app)
- 6DoF tracking (IMU-based, needs porting to new app)
- External gamepad as VR controllers

---

## Project structure

```
cardboardplusplus-master/
├── cardboardplusplus-android/    # Android VR app (Java + JNI/C++)
│   └── src/main/
│       ├── java/.../             # VideoDecoder, VideoManager, VrRenderer, etc.
│       └── jni/                  # Native: VideoReceiver, cardboard app
├── driver_cardboardplusplus/     # SteamVR driver (C++ DLL)
│   ├── src/                      # HmdDriver, VideoEncoder, ControllerDriver
│   ├── include/                  # Headers
│   └── lib/                      # FFmpeg + OpenVR SDK (pre-built)
├── sdk/                          # Google Cardboard SDK
├── proto/                        # Protobuf: Cardboard device params
├── third_party/                  # Unity XR Plugin API headers
├── CODEBASE_ISSUES.md            # Known issues and architectural problems
└── LICENSE                       # GPL v3
```

---

## How it works

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

### SteamVR driver

Open `driver_cardboardplusplus/driver_cardboardplusplus.sln` in Visual Studio. Build `Release|x64`.

Requires:
- FFmpeg (bundled in `lib/ffmpeg/`)
- OpenVR SDK (bundled in `include/` and `lib/`)

### Android app

Open `cardboardplusplus-android/` in Android Studio. Build and install on your phone.

Requires:
- Android SDK (API 24+)
- NDK (for native C++ code)
- Google Cardboard SDK (bundled)

---

## Contributing

This is a work in progress. **All contributions are welcome.**
See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

[GNU General Public License v3.0](LICENSE)

This project links against FFmpeg (libavcodec, libavutil, libswscale), which includes libx264. Since libx264 is GPL v2+, the combined work is licensed under GPL v3.
