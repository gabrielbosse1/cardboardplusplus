# SteamVR Driver Audit — `driver_cardboardplusplus/`
No code changes made. Research only.
Date: 2026-09-19. Agent: driver-audit.

## Files checked (33)
Read in full: docs/PROJECT_VISION.md, AGENTS.md, include/CardboardWire.h, include/BridgeProtocol.h, include/HmdDriver.h, include/VideoEncoder.h, include/VideoEncoderFFmpeg.h, include/VideoEncoderLog.h, include/H264Utils.h, include/BridgeServer.h, include/DebugLog.h, include/DriverLog.h, include/ControllerDriver.h, include/DeviceProvider.h, src/HmdDriver.cpp, src/Discovery.cpp, src/UdpTransport.cpp, src/EncoderSetup.cpp, src/EncodingThread.cpp, src/VideoEncoder.cpp, src/VideoEncoderFFmpeg.cpp, src/VideoEncoderShaders.cpp, src/DirectMode.cpp, src/DeviceProvider.cpp, src/DeviceFactory.cpp, src/ControllerDriver.cpp, src/DriverLog.cpp, src/BridgeServer.cpp, tests/test_wire_protocol.cpp, driver_cardboardplusplus.vcxproj, resources/driver.vrdrivermanifest, scripts/compile-driver.ps1, scripts/install-driver.ps1.

## HIGH severity

### H1. Phone quaternion written to legacy pose field; live rotation field hardcoded to identity
- File: src/HmdDriver.cpp:303-307 (GetPose)
- Evidence: pose.qDriverFromHeadRotation.w = 1.0 always; pose.qRotation = quat only.
- Why: compositor consumes qDriverFromHeadRotation; rotation may appear frozen in apps reading modern field.
- Fix: set pose.qDriverFromHeadRotation = quat; keep qRotation = quat. Verify with phone-rotate + SteamVR mirror before/after (see fix-risks.md R13).

### H2. m_streamEnabled stored but never read — stream can't be stopped
- File: stored src/HmdDriver.cpp:416; declared include/HmdDriver.h:217 (defaults 1=ON); zero read sites.
- Why: Bridge on/off switch is silent no-op; violates "Bridge is single switch".
- Fix: reset to 1 in Activate (else one OFF sticks across reloads — see fix-risks.md R3), then check flag in Present() only (single load) + log transitions. Fix M15 alongside so the later ON can't be skipped.

### H3. Telemetry callback set but never invoked — SHM telemetry chain dead
- File: src/VideoEncoder.cpp:265-267 setter, src/HmdDriver.cpp:462-466 registration; no call site.
- Why: BridgeServer::PublishTelemetry never called → hasTelemetry_ false → PublishStatus early-returns → SHM never carries anything.
- Fix: invoke m_telemetryCallback from LogTelemetrySummary() / FinishEncode(); test callback fires per interval.

### H4. Encoder re-inits drop telemetry callback
- File: src/EncoderSetup.cpp:168-170,226-228 only SetEncodedPacketCallback vs src/HmdDriver.cpp:459-466 sets both.
- Fix: RegisterEncoderCallbacks() helper setting both; call at all 4 init sites.

### H5. SHM settings path ApplyStreamSettings has zero validation
- File: src/HmdDriver.cpp:412-433. bitrate*1000 can overflow int; fps=0 → bad time_base; odd/zero W/H → avcodec_open2 fail → stream dead.
- Fix: reuse ApplyBridgeCfg validation (fps 1-120, sane bitrate, even W/H >=320x180, 16-align); reject-and-keep-old.

### H6. UDP video uses ~60KB datagrams — guaranteed IP fragmentation
- File: src/UdpTransport.cpp:31 chunkSize 60000. MTU ~1500 → ~40 fragments; loss of one = whole frame lost.
- Fix: chunk <=1400B (<=1200 for VPN/Tailscale) TOGETHER with Android 6.3 ring-buffer + M9 scratch reuse (see fix-risks.md R9) — chunks alone raise pps and worsen receiver memmoves.

### H7. No FFmpeg runtime DLLs exist — driver can't load / installer ships nothing
- File: scripts/install-driver.ps1:29-31; lib/ffmpeg/bin/ does NOT exist; vcxproj links avcodec/avformat/avutil/swscale.lib.
- Fix: vendor pinned FFmpeg shared DLLs, make install-driver.ps1 throw when no avcodec-*.dll etc found, log DLL set.

### H8. Sine-wave position bob injected on top of real gyro tracking
- File: src/HmdDriver.cpp:289-301 GetPose else branch.
- Fix: delete sine fallback; identity rotation or OutOfRange when !hasQ.

## MEDIUM
- M1 Ghost example controller ships (ControllerDriver.cpp:45-85, DeviceProvider.cpp:19-20) — hardcoded joystick Y 0.95 forward + sine bob as "example_controller". Fix: stop registering or gate behind debug flag; rename to cardboardplusplus_*.
- M2 GetProjectionRaw top/bottom look inverted (HmdDriver.cpp:375-382, top=-1 bottom=+1). Verify with test pattern; likely swap to top=+1 bottom=-1.
- M3 Encoder 2880x1620 default vs render 1920x1080 SBS (EncoderSetup.cpp:33-39 vs HmdDriver.cpp:355-359) — encodes interpolated pixels. Fix: default to render size, 16-align + even-check all writes.
- M4 Sticky poison hardware caps (EncoderSetup.cpp:186-200,84-112) — invalid capW/H stored forever; align-down can yield 0. Fix: validate 320-7680 even before storing; log malformed CARDBOARD_CAP (Discovery.cpp:144-148 silent today).
- M5 Duplicate control planes SHM + UDP BRIDGE_CFG with different validators (HmdDriver.cpp:412-479 vs EncoderSetup.cpp:114-184). Fix: single ApplyEncoderSettings() with one validator.
- M6 Dead readback+convert pipelines allocated every init (VideoEncoder.h, VideoEncoderFFmpeg.cpp:334-390 FinishFrame, VideoEncoderShaders.cpp). Fix: delete dead methods + unused staging textures; fix header comment.
- M7 VideoEncoder::Shutdown leaks CPU readback buffer per re-init (VideoEncoderShaders.cpp:424-429 new[], VideoEncoder.cpp:150-178 no delete[]; ~18MB per re-init). Fix: delete[] in Shutdown().
- M8 m_readbackRowPitch written never consumed; SwsConvert assumes packed pitch. Fix: assert pitch==width*4 or pass explicitly; delete unused member.
- M9 Per-frame OpenSharedResource + malloc per encoded frame (EncodingThread.cpp:53-57, VideoEncoder.cpp:180-213, UdpTransport.cpp:142-167). Fix: cache opened textures; reuse scratch frame buffer.
- M10 Discovery accepts ANY packet as phone (Discovery.cpp:210-222) + ACK to scanners. Fix: allowlist CARDBOARD_DISCOVERY (+ PHONE_HELLO variants) for target switch; log-and-rate-limit unknown instead of ACK-switch (see fix-risks.md R2). Update test_wire_protocol.cpp:560-566 which asserts any-packet ACK.
- M11 m_serverAddr written under mutex, read lock-free (Discovery.cpp:289-312 vs UdpTransport.cpp:184-194). Fix: snapshot under mutex or atomic packed IP+port.
- M12 CreateSwapTextureSet leaks partial textures + hands zeros to SteamVR (DirectMode.cpp:69-102). Fix: rollback on failure.
- M13 Installer gaps: resources/*.json never copied, no VC++ redist check, no vrserver lock handling, single .bak, no enablement in vrsettings. Fix: copy resources tree, throw on missing redist, retry/backoff copy, versioned backups.
- M14 m_udpFramesSent double-counts (UdpTransport.cpp:177-178,195 +1 per copy); drops conflate preview+phone. Fix: count once per OnEncodedPacket; separate droppedPreview/Phone.
- M15 SHM PollSettings can skip newest forever (BridgeServer.cpp:168-197). Fix: resync to ws-1 on invalid slot; or periodic re-publish.

## LOW
L1 stale joke comment HmdDriver.cpp:27-32 — delete. L2 GetPose comment misnames 0x10 vs 0x12 (HmdDriver.cpp:259). L3 stale file headers (UdpTransport.cpp:11-18, HmdDriver.h:68-69, VideoEncoder.h:48-51, VideoEncoderFFmpeg.cpp:11-14, test_wire_protocol.cpp:568-590). L4 CardboardWire.h:5-10 omits kSensorPort/KEYFRAME_REQ/BRIDGE_*. L5 Dead BridgeServer API PublishPose/FrameSubmitted/etc (BridgeServer.h:46,48-51). L6 unused LoadAcquireU64, hasS, stubs, commented block. L7 hand-rolled AppendPaddedNumber (Discovery.cpp:251-287) → single snprintf. L8 busy-spin producer lock (BridgeServer.cpp:205-206). L9 BuildLengthPrefixedPacket null unchecked (H264Utils.h:12-25, UdpTransport.cpp:149,163). L10 BRIDGE_PREVIEW strstr "1" (Discovery.cpp:173) — parse token explicitly. L11 unchecked inet_pton / pIndices / 16-layer cap. L12 committed binaries (test_wire_protocol.exe/.obj, x64/, driver_c.e9f07c0e/) + no test target in vcxproj though AGENTS.md claims post-build tests. L13 broken Win32 configs in vcxproj — delete. L14 Activate doesn't reset session state; Deactivate order. L15 IPv4-only, localhost preview, no firewall rules, pinned old OpenVR interfaces (_007/_004).

Suggested order: H1+H8 → H6 → H3+H4 → H2 → H5+M4+M3 → H7+M13 → M7/M14/M15/M10/M11/M9 → M1 → M6/L3/L12-L13.
