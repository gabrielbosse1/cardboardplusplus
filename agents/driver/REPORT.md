# Driver Agent — Performance + Logic Audit

Scope: `driver_cardboardplusplus/` only.
Files scanned: 27 (14 src .cpp, 12 headers, 1 test; vendored openvr/vulkan/ffmpeg excluded).
Findings: 10 critical, 18 major, 19 minor — total 47.

Wire contract: values match AGENTS.md (kDataPort 42069, kDiscoveryPort 42070, literals + lengths correct). Only length comments wrong, kSensorPort 42074 undocumented.

## Critical

- `driver_cardboardplusplus/src/UdpTransport.cpp:177` | critical | logic | SendFannedOut always sends localhost preview, ignores m_previewEnabled. Fix: skip preview send when false.
- `driver_cardboardplusplus/include/DebugLog.h:21` | critical | perf | debug_enabled() always true, every DebugLog does VRDriverLog file IO on hot paths. Fix: false unless CARDBOARD_DEBUG=1.
- `driver_cardboardplusplus/src/HmdDriver.cpp:292` | critical | logic | GetPose identity qDriverFromHeadRotation vs sensor qRotation contradiction. Fix: assign sensor quat to both.
- `driver_cardboardplusplus/src/HmdDriver.cpp:301` | critical | perf | GetPose DebugLog every call, SteamVR polls fast. Fix: delete or throttle.
- `driver_cardboardplusplus/src/ControllerDriver.cpp:45` | critical | logic | Sine-wave bob + joystick 0.95 hardcoded, avatar drifts/walks. Fix: drive from bridge hand data.
- `driver_cardboardplusplus/src/VideoEncoderFFmpeg.cpp:23` | critical | logic | Codec loop picks first encoder then overwrites m_useGpuEncoding, bridge selection ignored. Fix: honor requested flag.
- `driver_cardboardplusplus/src/BridgeServer.cpp:231` | critical | logic | PublishStatus zeroed MT_TELEMETRY every second corrupts averages. Fix: distinct type or real counters.
- `driver_cardboardplusplus/src/HmdDriver.cpp:154` | critical | logic | Deactivate never ShutdownBridge, SHM leak. Fix: call ShutdownBridge.
- `driver_cardboardplusplus/src/HmdDriver.cpp:192` | critical | logic | DestroyAllSwapTextureSets(0) keyed by pid, nothing freed. Fix: iterate pids or destroy-all overload.
- `driver_cardboardplusplus/src/DirectMode.cpp:64` | critical | logic | CreateSwapTextureSet failure leaves pOut garbage. Fix: zero pOut on entry.

## Major

- `driver_cardboardplusplus/src/UdpTransport.cpp:178` | major | logic | m_udpFramesSent +1 per send (preview+phone) = 2x count. Fix: +1 per encoded frame.
- `driver_cardboardplusplus/include/HmdDriver.h:235` | major | logic | m_udpDroppedFrames plain uint32 cross-thread race, mixes targets. Fix: atomic or per-target.
- `driver_cardboardplusplus/src/Discovery.cpp:258` | major | logic | SendBridgeStats reads fps/bitrate unlocked vs mutex write. Fix: snapshot under mutex or atomic.
- `driver_cardboardplusplus/src/UdpTransport.cpp:193` | major | logic | SendFannedOut reads m_serverAddr unlocked vs SwitchDataTarget lock. Fix: copy under mutex.
- `driver_cardboardplusplus/src/EncodingThread.cpp:54` | major | perf | OpenSharedEyeTextures re-opens per layer per frame. Fix: cache, reopen on change.
- `driver_cardboardplusplus/src/VideoEncoderShaders.cpp:563` | major | perf | ComposeSBSLayer creates/releases 2 SRVs + Map/Unmap per layer per frame. Fix: cache SRVs, upload once.
- `driver_cardboardplusplus/src/VideoEncoderShaders.cpp:405` | major | perf | ReadbackToBuffer + SwsConvert full-frame memcpy (~18MB @2880x1620) vs zero-copy path. Fix: route via FinishFrame.
- `driver_cardboardplusplus/src/VideoEncoderFFmpeg.cpp:43` | major | perf | gop 10 @60fps = keyframe every 166ms, bandwidth bloat. Fix: ~2x fps + RequestKeyframe.
- `driver_cardboardplusplus/src/HmdDriver.cpp:408` | major | perf | ApplyStreamSettings re-inits FFmpeg sync on compositor thread, stalls seconds. Fix: defer to encode thread.
- `driver_cardboardplusplus/src/DirectMode.cpp:514` | major | logic | WaitEncoderIdle ignores 2s timeout, use-after-free risk. Fix: check result, abort/retry.
- `driver_cardboardplusplus/src/Discovery.cpp:201` | major | logic | Unknown packet on 42070 switches target + ACK, hijackable. Fix: accept known prefixes only.
- `driver_cardboardplusplus/src/HmdDriver.cpp:232` | major | logic | No prediction, zero vel/angvel = judder. Fix: extrapolate to photon time, fill vel.
- `driver_cardboardplusplus/src/BridgeServer.cpp:143` | major | perf | EnsureCmdMapping OpenFileMappingW every RunFrame until bridge up. Fix: backoff ~1Hz.
- `driver_cardboardplusplus/src/VideoEncoderFFmpeg.cpp:84` | major | logic | av_opt_set unchecked, bad preset silently defaults (nvenc no fast). Fix: check + log.
- `driver_cardboardplusplus/src/DirectMode.cpp:460` | major | logic | EnsureLayerCopies only checks left eye, right mismatch = copy size error. Fix: compare both.
- `driver_cardboardplusplus/src/HmdDriver.cpp:284` | major | logic | Fresh data no quat = synthetic sine bob. Fix: identity position.
- `driver_cardboardplusplus/src/UdpTransport.cpp:142` | major | perf | malloc/free framed buffer every frame +2 per keyframe. Fix: persistent scratch.
- `driver_cardboardplusplus/src/VideoEncoderFFmpeg.cpp:488` | major | perf | malloc SPS+PPS + AVPacket per keyframe. Fix: persistent buffer/packet.

## Minor

- `driver_cardboardplusplus/include/CardboardWire.h:37` | minor | logic | Length comments wrong (12 not 11, 12 not 13, 14 not 13). Fix: correct comments.
- `driver_cardboardplusplus/src/Discovery.cpp:173` | minor | logic | BRIDGE_PREVIEW strstr 1, "10" enables. Fix: parse int.
- `driver_cardboardplusplus/src/Discovery.cpp:138` | minor | logic | CAP-only phone never sets video target. Fix: refresh target on CAP.
- `driver_cardboardplusplus/src/HmdDriver.cpp:251` | minor | logic | GetTickCount64 + steady_clock mixed. Fix: steady_clock both.
- `driver_cardboardplusplus/src/HmdDriver.cpp:636` | minor | logic | Quats never normalized/validated, NaN reaches SteamVR. Fix: reject zero-norm, normalize.
- `driver_cardboardplusplus/src/EncoderSetup.cpp:83` | minor | logic | ClampEncoderToCap only shrinks, never grows. Fix: clear on settings change.
- `driver_cardboardplusplus/src/VideoEncoderFFmpeg.cpp:328` | minor | perf | FinishFrame/FinishEncode dupes, dead paths. Fix: merge, delete dead.
- `driver_cardboardplusplus/src/VideoEncoderShaders.cpp:281` | minor | perf | 3 staging textures alloc never used. Fix: delete.
- `driver_cardboardplusplus/include/VideoEncoder.h:174` | minor | perf | m_pD3dMutex unused. Fix: delete.
- `driver_cardboardplusplus/src/DirectMode.cpp:360` | minor | perf | Present Flush per layer copy stalls GPU. Fix: remove if fenced.
- `driver_cardboardplusplus/src/DeviceProvider.cpp:20` | minor | logic | example_controller/example_virtual_hmd names. Fix: rename cardboardplusplus.
- `driver_cardboardplusplus/src/HmdDriver.cpp:96` | minor | perf | No SetThreadPriority, encode competes normal. Fix: raise encode priority.
- `driver_cardboardplusplus/src/DirectMode.cpp:523` | minor | logic | GetFrameTiming empty, pacing blind. Fix: report real timing.
- `driver_cardboardplusplus/src/Discovery.cpp:135` | minor | perf | DriverLog every discovery %s (binary risk). Fix: gate debug, log len.
- `driver_cardboardplusplus/src/HmdDriver.cpp:662` | minor | logic | Shutdown wake 0x00 logged unknown every shutdown. Fix: ignore silently.
- `driver_cardboardplusplus/driver_cardboardplusplus/tests/test_wire_protocol.cpp:117` | minor | logic | BRIDGE_STATS string vs cfg input, ignores result. Fix: parse BRIDGE_CFG, assert.
- `driver_cardboardplusplus/driver_cardboardplusplus/tests/test_wire_protocol.cpp:132` | minor | logic | Compares vs kBridgeHeartbeatLen not kCardboardCapLen. Fix: use CapLen.
- `driver_cardboardplusplus/include/CardboardWire.h:69` | minor | logic | kSensorPort 42074 absent from AGENTS.md. Fix: document 42074.
- `driver_cardboardplusplus/src/HmdDriver.cpp:249` | minor | logic | 2s timeout +2s stale = ~4s dead pose. Fix: 500ms timeout.
