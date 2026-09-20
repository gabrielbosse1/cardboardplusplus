# Android Client Audit — `cardboardplusplus-android`
Research only. No code changed. Date: 2026-09-19.

## Files checked (45)
Java main (24): core/AppConstants, core/DebugLog, VrActivity, NativeBridge, telemetry/TelemetrySender, discovery/DiscoveryManager, video/VideoManager,VideoDecoder,VideoWatchdog,H264NalParser,DecoderCapabilityReporter,NetStatsReporter, streaming/CameraStreamer, camera/CameraController,CameraUtils, network/NetworkUtils, settings/AppSettings,SettingsMenuController, render/VrRenderer,FpsCounter, codec/CodecSelector,VideoCodec, permissions/PermissionManager, ui/ImmersiveMode. tracking/ empty (0 files). JNI (8): VideoReceiver.h/cpp, H264Decoder.h/cpp, app.h/app.cc/jni.cc, util.h/cc. jni/sixdof/ empty. Tests (8): WireContract, CrossComponentContract, TelemetryEncode, NetStats, CameraStreamer, DiscoveryManager, DecoderCapability, H264NalParser. Build (5): build.gradle, CMakeLists, AndroidManifest, compile-app.ps1, install-app.ps1.

## 1. Comments
- 1.1 VrActivity.java:84-86 "neither is started" false — startSession starts telemetry (256) + camera (279). MED.
- 1.2 VideoReceiver.h:40 "first NAL SPS/IDR" false — impl scans whole payload (VideoReceiver.cpp:200-215) because AUD(9) first. MED.
- 1.3 app.h:171-175 same wrong first-NAL claim; impl .cc:314-328 scans whole. MED.
- 1.4 app.h:122-129 DecodeLoop YUV->RGBA claim false — .cc:742-793 only forwards to Java MediaCodec. LOW.
- 1.5 DecoderCapabilityTest.java:55-56 constants in wrong file (actually DiscoveryManager.java:33-34). LOW.
- 1.6 WireContractTest.java:74 "ports not in AppConstants" false (AppConstants.java:14-15 has them); test hardcodes dups. LOW.
- 1.7 CrossComponentContractTest.java:180-183 assertEquals(42072,42072) tautology; CameraStreamer.PC_PORT doesn't exist. Fix assert AppConstants.CAMERA_PORT. MED.
- 1.8 CameraStreamerTest vs CameraStreamer.java:148 off-by-two (test length>60000 vs prod length+2>60000). LOW.
- 1.9 DecoderCapabilityTest blesses "CARDBOARD_CAP 0 0" but prod guards w>0&&h>0 (DiscoveryManager.java:126). LOW.

## 2. Dead code
- 2.1 tracking/ empty — delete or README/package-info "reserved". LOW.
- 2.2 jni/sixdof/ empty — same. LOW.
- 2.3 HIGH: H264Decoder.h/cpp ~470 lines dead — Initialize/DecodePacket/HasDecodedFrame/GetRGBAFrame zero call sites (constructed .cc:207 + Shutdown .cc:715 only). Live path receiver→DecodeLoop→Java MediaCodec. Delete + re-evaluate ffmpeg-kit dep (GPL + size).
- 2.4 Dead RGBA double-buffer fields app.h:257-274 (video_buf_a/b, video_latest etc) — no refs in .cc. Delete.
- 2.5 Dead JNI NativeBridge.java:41,47,49 setEyeTexture/updateVideoTexture/hasVideoFrame no Java callers; native UpdateVideoTexture no-op .cc:735-740. Delete.
- 2.6 Write-only codec/settings plumbing VrActivity:86,123 CodecSelector.select never called; AppSettings getters/setters no callers; settings_menu only viewer/PC-IP/debug; HEVC/AV1 never decodable (AVC-only native). Delete or wire to CAP. MED.
- 2.7 DebugLog.create() null path only (TelemetrySender:40, DiscoveryManager:28 pass null); setEnabled writes global. Delete or fix. LOW.
- 2.8 Dead util RandomUniform/AngleBetween/GetMatrixFromGlArray/Texture class — delete sample ballast. LOW.
- 2.9 Unused deps build.gradle:44-51 gms/protobuf/material zero imports; play-services-vision TODO migrate. Remove (APK size + policy). MED.

## 3. Redundant work
- 3.1 HIGH: every frame keyframe-scanned 3-4x — VideoReceiver.cpp:204-215 + DecodeLoop IsKeyframe .cc:771 + VideoDecoder.feedFrame extractNal x2 (VideoDecoder.java:142-143) each copyOfRange-allocating. Fix: compute is_key once in receiver, pass boolean (feedFrame already takes isKey), skip rescan except reconfig path.
- 3.2 CAP burst blocks discovery 1.5s (DiscoveryManager.java:157-171 3x sleep 500 on discovery thread). Send once per event or burst on short thread. MED.
- 3.3 extractNal copies + full SPS compare per keyframe (VideoDecoder.java:144-148). Compare dims first. LOW/MED.
- 3.4 per-iteration discovery allocs + InetAddress.getByName every 500ms (DiscoveryManager.java:100-110). Cache until pcIp changes. LOW.

## 4. Library/API
- 4.1 HIGH: global broadcast 255.255.255.255 (NetworkUtils.java:16-18) dropped by APs/client-isolation, no broadcast domain on Tailscale/VPN. Fix: subnet-directed broadcast via DhcpInfo, fallback global; consider NsdManager/mDNS.
- 4.2 hand-rolled SPS parser ignoring crop (H264NalParser.java:74-143) returns coded not visible dims; papered by buggy vMax shader clamp. Fix crop or use MediaFormat KEY_WIDTH/HEIGHT/CROP_* after configure. MED.
- 4.3 deprecated SYSTEM_UI_FLAG_* (ImmersiveMode.java:17-24) on targetSdk 35. WindowInsetsController on 30+. LOW.
- 4.4 unvalidated IP input (SettingsMenuController.java:82-89). Validate InetAddress on OK. LOW/MED.

## 5. Round-trips
- 5.1 Discovery broadcasts forever after ACK (DiscoveryManager.java:96-150 broadcasting never cleared; test FakeDiscovery DOES stop — divergence). Fix: back off to low-rate heartbeat (1 per 5s) after first ACK, resume full rate on watchdog stall / 5s without ACK; keep CAP re-announce every 60 ACKs (see fix-risks.md R1). Never stop fully — driver restart clears target and needs rediscovery.
- 5.2 0x10 @~200Hz diagnostics-only yet full-rate (TelemetrySender.java:248-300; AGENTS.md mag/0x10 diagnostics, driver tracks 0x12). Throttle 0x10 to ~10Hz. MED.
- 5.3 0x11/0x20 never sent (zero send sites; only tests). Send hints or delete test surface. LOW.
- 5.4 fresh socket per NetStats (NetStatsReporter.java:111-114) + DNS every 2s. Reuse TelemetrySender socket. Note wall-clock vs boot-ms (§7.1). LOW/MED.
- 5.5 full-frame JNI copy per video frame (app.cc:773-778 NewByteArray+SetRegion multi-MB @2880). Direct ByteBuffer once or feed MediaCodec natively. MED.

## 6. Perf
- 6.1 HIGH sensor hot-path on MAIN thread (no Handler): ByteBuffer.allocate 45|25 + executor.execute + new DatagramPacket per event up to ~400/s; getDisplayRotation getSystemService+getRotation per packet (230,270); qmul 3x float[4] + remap 3x float[3] per rotation. Fix: reused buffers, cache rotation (invalidate onConfigChanged), static qmul(out), HandlerThread.
- 6.2 camera ~5 allocs/frame @30fps (CameraStreamer.java:138-157,200-239 NV21 full+scaled+BAOS+jpeg+payload + per-pixel Java loops + compressToJpeg). Request smaller directly, reuse buffers / libyuv. MED.
- 6.3 receiver memmoves + 24-frame queue (VideoReceiver.cpp:167,197-198,222-251 insert/erase O(n) up to 16MB; 24 frames ~400ms+MBs). Ring/offset buffer, queue 6-8, earlier desync on huge frame_len. MED.
- 6.4 decoder/GL lock + 10ms input wait (VideoDecoder.java:76,135,174 synchronized both threads; dequeueInputBuffer 10s). Fix: narrow sync; drop P-frames only with dequeue 0, never drop IDR/SPS, keep short wait for keyframes (see fix-risks.md R11). MED.
- 6.5 queryDecoderCap on UI thread (VrActivity.java:132 MediaCodecList.ALL). Background thread. MED ANR.
- 6.6 stopDiscovery joins 2s on UI (DiscoveryManager.java:79-89 via onPause:184). Off-UI join. MED ANR.
- 6.7 FpsCounter Log.i every 1s (render/FpsCounter.java:30). Gate. LOW.
- 6.8 1ms spin DecodeLoop (app.cc:765-768). Condvar/blocking queue. LOW.

## 7. Logic bugs
- 7.1 MED/HIGH mixed epochs port 42071: 0x10/0x12 boot-ms (TelemetrySender:263,308 event.timestamp) vs 0x13 wall-clock (NetStatsReporter:110 currentTimeMillis). Bridge forwards verbatim (telemetry.rs, phone.rs:186-210). Fix 0x13 to elapsedRealtime. Quat order OK [w,x,y,z]; Q_WORLD Rx(-90) correct.
- 7.2 HIGH vMax wrong height after res change (VideoDecoder.java:214-215 vs 241): uses construction height not baseH. Fix vMax=(float)baseH/alignedH. Green-line regressor.
- 7.3 HIGH decoder cap floor ratchets wrong (DecoderCapabilityReporter.java:31-32,51-53 maxW/H start 1920 only increase, max across decoders). Announces >=1920 on weak phones → black screen. Fix: cap of the decoder MediaCodec will select (by name), or min across realtime-capable HW AVC decoders; never mix W+H from different decoders (see fix-risks.md R8).
- 7.4 MED 3 default resolutions: AppSettings 2880x1620 vs AppConstants 1920x1080 vs native 2880x1620 (app.cc:193-194); VideoManager 1920x1080; settings inert. Single source; native members write-only.
- 7.5 MED double onSurfaceCreated leaks decoder (VrRenderer:37-38 + VrActivity:262-268 race). Guard in VideoManager (release-then-create).
- 7.6 NAL scanners miss tail (H264NalParser:32,60 i+4<n / i+3<len). Scan to n. LOW.
- 7.7 NetStats delta spikes after recreate (NetStatsReporter:98-102 lastTotal not reset → negative fps). Clamp/reset. LOW.
- 7.8 throttle timestamp race benign + main-thread (TelemetrySender:62-63,248-252). HandlerThread. LOW.
- 7.9 MED camera-denied dead-ends (VrActivity:400-409 toast only vs storage path 390-395 redirect). Mirror rationale→settings.
- 7.10 test/prod ACK divergence (DiscoveryManagerTest Fake stops; prod never). Align with 5.1.

## 8. Resolution/fallbacks
V1 conflicting defaults (§7.4). V2 1920 floor + max (§7.3). V3 CAP clamp direction correct (phone ceiling, driver clamps); "every ~30s" actually every 60 ACKs — document. V4 DEFAULT_CAMERA 640x480 + 256x192 q38 correct; chooseOutputSize queries SurfaceTexture sizes but ImageReader YUV_420_888 may differ → use YUV sizes (CameraController:152-155). V5 default PC IP "" broadcast sane; 5-fail fallback good.

## 9. Packaging
- 9.1 HIGH full-gpl ffmpeg-kit (build.gradle:51 full-gpl-16kb:6.1.4) only for dead decoder — tens MB/ABI + GPLv3 obligations. Drop to min/https or remove (keep 16kb prop for API35+).
- 9.2 no ABI splits, debug-only (build.gradle:23-25,30-35, compile-app.ps1:5 assembleDebug). Splits/Bundle + release path. MED.
- 9.3 CMake cross-include driver lib (CMakeLists:53-54) breaks standalone checkout. Vendor headers or gate on H264Decoder deletion. MED.
- 9.4 manifest hygiene (AndroidManifest:5-11): READ/WRITE_STORAGE no maxSdkVersion (code gates pre-Q only VrActivity:331; target 35 inert) → Play warnings; allowBackup true backs up PC IP; missing configChanges on landscape VR → foldable resize kills session. Fix maxSdkVersion 28/remove WRITE, allowBackup false/rules, add configChanges. MED.
- 9.5 install-app.ps1 device ambiguity (27-31 -match device passes multi-device then adb install fails no -s; no -g). Fail >1 with -s guidance, add -g. LOW.
- 9.6 uses-feature gyro+accel required true excludes gyro-less (preview-capable); magnetometer correctly optional. Keep (tracking mandatory) — deliberate. LOW.

## 10. Setup breakage
10.1 HIGH broadcast dies (subnets/VLANs, isolation, Tailscale no broadcast) — unicast PC-IP buried in submenu. Directed-broadcast + first-run PC-IP affordance + mDNS. 10.2 IPv6/hostname (getPcOrBroadcastAddress may return v6 vs AF_INET VideoReceiver.cpp:31) — prefer IPv4. MED. 10.3 MTU 64KB video (VideoReceiver.h:46) + 60KB camera (AppConstants:38) → ~40 fragments; video desync buffer.clear + KEYFRAME_REQ throttled good; camera no retry by design. Don't raise res without chunking. MED. 10.4 SoC quirks c2.qti assumptions (VideoDecoder:57), Annex-B csd, ALLOW_FRAME_DROP vs watchdog, setDefaultBufferSize after configure — test matrix; MediaFormat dims removes quirk. MED. 10.5 Doze (no FGS/exemption) throttles UDP/sensors backgrounded; wake/wifi locks mask in-viewer only. Document/prompt. MED. 10.6 version drift getDefaultDisplay deprecated 30, SYSTEM_UI_FLAG, SCREEN_BRIGHT_WAKE_LOCK (VrActivity:295 use PARTIAL+FLAG_KEEP_SCREEN_ON already set), storage perms; minSdk26 fine. LOW/MED. 10.7 portrait/landscape coherent (manifest landscape + remap 90/270/180/0); no recenter; tablets ROTATION_0 edge. LOW. 10.8 camera-denied trap (§7.9). MED.

Priority: §7.2/7.3/7.1 → §2.3+9.1/2.4-2.6/2.9 → §6.1/3.1/6.3 → §10.1/4.1/7.9/9.4 → §1/7.10.
