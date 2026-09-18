# App Agent — Performance + Logic Audit

Scope: `cardboardplusplus-android/` only.
Files scanned: ~34 (22 Java main, 7 JNI C/C++/H, build.gradle, CMakeLists.txt, AndroidManifest.xml, 4 contract tests).
Findings: 5 critical, 21 major, 34 minor — total 60.

Wire-contract: 42069 video, 42070 discovery/CAP/KEYFRAME_REQ, 42071 telemetry/hello, 42072 camera JPEG all correct ports. Exceptions: 0x12 undocumented, 60KB single-datagram MTU risk.

## Critical

1. `src/main/jni/VideoReceiver.cpp:204-206` | critical | logic | 4-byte start-code check requires `00 00 01 01`, real code is `00 00 00 01`, so 4-byte SPS/IDR never detected as key. Fix: check `frame[i+2]==0x00 && frame[i+3]==0x01`.
2. `src/main/java/com/google/cardboard/VrActivity.java:234-269,90-98` + `render/VrRenderer.java:33-45` | critical | logic | `VideoManager.start()` only in `onSurfaceCreated`; after pause/resume with preserved GL context receiver never restarts. Fix: call `videoManager.start()` guarded from `startSession()` after `glView.onResume()`.
3. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:65,122-133` | critical | logic | `stop()` calls `executor.shutdownNow()` and `start()` never recreates it, pause/resume throws `RejectedExecutionException`. Fix: recreate ExecutorService in `start()`.
4. `src/main/java/com/google/cardboard/video/VideoDecoder.java:75-113,128-181,246-276` | critical | logic | `updateVideoTexture()` (GL thread) unsynchronized while `feedFrame()`/`release()` on other threads, pause can release MediaCodec mid-dequeue. Fix: synchronize `updateVideoTexture()` on same monitor or release on GL thread.
5. `src/main/jni/cardboardplusplus_app.cc:690-714` | critical | logic | `StopVideoReceiver()` calls `glDeleteTextures()` from UI thread with no GL context. Fix: delete texture on GL thread.

## Major

6. `src/main/java/com/google/cardboard/video/VideoDecoder.java:226-236` | major | logic | `vMax` uses field `height` (1080) instead of stream `baseH`, wrong crop for 2880x1620. Fix: `vMax = (float) baseH / alignedH`.
7. `src/main/java/com/google/cardboard/discovery/DiscoveryManager.java:137-143` | major | logic | After 5 timeouts user PC IP silently wiped with `setPcIp("")`. Fix: keep setting, use broadcast temporarily.
8. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:148-155` | major | logic | `CARDBOARD_PHONE_HELLO` sent once per socket, restarted bridge never relearns IP. Fix: re-send hello every 2-5s.
9. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:28-42` | major | logic | Tag `0x12` rotation packets undocumented in AGENTS.md (only 0x10/0x11/0x20/hello). Fix: document 0x12, verify bridge parses it.
10. `src/main/java/com/google/cardboard/core/AppConstants.java:38` + `streaming/CameraStreamer.java:148` | major | perf/logic | Camera JPEG up to 60000B in one UDP datagram (~40 fragments). Fix: cap near ~1400B with chunking or lower quality.
11. `src/main/java/com/google/cardboard/video/VideoManager.java:70` + `video/VideoDecoder.java:43-49` | major | logic | Watchdog `decoderSupplier` reads `VideoManager.decoder` cross-thread non-volatile, `running` non-volatile. Fix: make both volatile.
12. `src/main/jni/cardboardplusplus_app.cc:728-779` | major | perf/logic | `DecodeLoop` holds `video_decoder_obj_` from thread start, feeds stale object after resume. Fix: re-fetch decoder handle each iteration.
13. `src/main/jni/cardboardplusplus_app.cc:748-754` | major | perf | `DecodeLoop` polls `GetFrame()` with 1ms sleep (~1000 wakeups/s idle). Fix: condvar/blocking queue signaled by VideoReceiver.
14. `src/main/jni/cardboardplusplus_app.cc:759-766` | major | perf | `NewByteArray` + copy per frame at 60fps. Fix: reused direct ByteBuffer.
15. `src/main/jni/VideoReceiver.cpp:167,197-198` | major | perf | Reassembly `vector::insert` + `erase(begin,...)` O(n) memmoves on MB buffers. Fix: offset-index ring buffer.
16. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:277,306` | major | perf | `ByteBuffer.allocate(45/25)` + DatagramPacket + Runnable per sensor event (~200-400/s). Fix: ThreadLocal reuse, send inline.
17. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:204-211,224-236` | major | perf | `qmul()` allocates 3-4 float[4] per rotation (~200Hz). Fix: static scratch arrays.
18. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:266,328-337` | major | perf | `getDisplayRotation()` binder calls per sensor event. Fix: cache, update on config change.
19. `src/main/java/com/google/cardboard/streaming/CameraStreamer.java:138-156,200-239` | major | perf | `sendFrame()` allocates NV21 + downscaled + BAOS + copies per frame at 30fps. Fix: reuse scratch buffers.
20. `src/main/java/com/google/cardboard/streaming/CameraStreamer.java:230-236` | major | perf | Per-pixel `ByteBuffer.get()` (~76k/frame). Fix: bulk row copies.
21. `src/main/java/com/google/cardboard/video/VideoDecoder.java:135-152` | major | perf | `feedFrame()` two full NAL scans + SPS parse per unit. Fix: single-pass,parse SPS only on change.
22. `src/main/jni/VideoReceiver.cpp:183-190` | major | logic | Length mismatch clears whole buffer, one loss drops all partials. Fix: scan forward for next length.
23. `src/main/java/com/google/cardboard/VrActivity.java:132` + `video/DecoderCapabilityReporter.java:30-60` | major | perf | `queryDecoderCapability()` on UI thread (~100-200ms). Fix: off-thread + cache.
24. `src/main/java/com/google/cardboard/discovery/DiscoveryManager.java:79-89` | major | logic | `stopDiscovery()` `join(2000)` on main thread blocks UI 2s. Fix: interrupt without join on UI.
25. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:136-169` | major | logic | `connectLoop()` new DatagramSocket per retry without close, fd leak. Fix: close previous before rebind.
26. `src/main/java/com/google/cardboard/video/VideoDecoder.java:167-177` | major | logic | `dequeueInputBuffer(10000)` blocks 10ms, pts always 0. Fix: short timeout + monotonic pts.

## Minor

27. `src/main/java/com/google/cardboard/video/H264NalParser.java:80-81` | minor | perf | RBSP `new byte[n]` per SPS parse. Fix: parse in place.
28. `src/main/java/com/google/cardboard/video/H264NalParser.java:29-47,59-65` | minor | logic | `extractNal` `i+4<n` misses last 4 bytes. Fix: `i+3<=n`.
29. `src/main/java/com/google/cardboard/video/VideoDecoder.java:91-100` | minor | perf | Per-second DBG varargs on GL thread. Fix: guard with `isEnabled()`.
30. `src/main/java/com/google/cardboard/core/DebugLog.java:56-77` | minor | perf | varargs alloc even when disabled. Fix: guard call sites.
31. `src/main/java/com/google/cardboard/core/DebugLog.java:28-31` | minor | logic | `setEnabled()` flips static global. Fix: separate setters.
32. `src/main/java/com/google/cardboard/discovery/DiscoveryManager.java:157-171` | minor | perf | `sendCap()` 3x500ms sleeps stall discovery 1.5s. Fix: send without sleeps.
33. `src/main/java/com/google/cardboard/discovery/DiscoveryManager.java:115` | minor | logic | `new String(bytes)` default charset. Fix: US_ASCII/UTF_8.
34. `src/main/java/com/google/cardboard/streaming/CameraStreamer.java:106-108` | minor | logic | `currentTimeMillis` + non-volatile throttle. Fix: `elapsedRealtime()` + volatile.
35. `src/main/java/com/google/cardboard/streaming/CameraStreamer.java:172-198` | minor | perf | Int divides per pixel. Fix: x/y LUTs.
36. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:287,314` | minor | logic | No backpressure, stale poses queue. Fix: drop-oldest/coalesce.
37. `src/main/java/com/google/cardboard/telemetry/TelemetrySender.java:102-107` | minor | perf | Mag at GAME (~200Hz) diagnostics-only. Fix: SENSOR_DELAY_NORMAL.
38. `src/main/java/com/google/cardboard/camera/CameraController.java:153-155,210-216` | minor | perf | Double-brace ArrayList per session. Fix: `Arrays.asList()`.
39. `src/main/java/com/google/cardboard/camera/CameraController.java:228-276` | minor | logic | Release outside `cameraLock` TOCTOU. Fix: release under lock.
40. `src/main/java/com/google/cardboard/camera/CameraController.java:156-171` | minor | logic | maxImages 2 + silent drops. Fix: count drops.
41. `src/main/java/com/google/cardboard/render/VrRenderer.java:53-58` | minor | perf | No thread priority. Fix: URGENT_DISPLAY/AUDIO.
42. `src/main/jni/cardboardplusplus_app.cc:596-612,614-631,393` | minor | perf | LOGD + glGetError per eye per frame. Fix: remove/gate debug.
43. `src/main/jni/cardboardplusplus_app.cc:330-394` | minor | perf | Clear each eye twice + blend toggles. Fix: clear once, set state once.
44. `src/main/jni/cardboardplusplus_app.cc:285-301` | minor | logic | SetVideoDecoder leaks global ref per resume. Fix: DeleteGlobalRef in Stop.
45. `src/main/jni/VideoReceiver.cpp:254` | minor | perf | LOGD per frame at 60fps. Fix: rate-limit.
46. `src/main/jni/VideoReceiver.h:46` | minor | perf | kMaxPacketSize 65536 > 65507 max UDP. Fix: 65507 + MSG_TRUNC check.
47. `src/main/jni/H264Decoder.cpp:66-223` | minor | logic | Failed Initialize leaks dlopen. Fix: centralize cleanup.
48. `src/main/jni/H264Decoder.cpp:266-315` | minor | logic | ConvertFrame ignores actual frame size. Fix: realloc on change.
49. `src/main/jni/cardboardplusplus_app.cc:308-322` | minor | logic | IsKeyframe differs from VideoReceiver. Fix: share detector.
50. `src/main/java/com/google/cardboard/network/NetworkUtils.java:21-26` | minor | perf | DNS per discovery loop. Fix: cache.
51. `src/main/java/com/google/cardboard/settings/AppSettings.java:37-45` | minor | logic | Dead resolution/fps/bitrate setters. Fix: remove or wire to CAP.
52. `src/main/java/com/google/cardboard/VrActivity.java:277-289` | minor | perf/logic | Deprecated SCREEN_BRIGHT_WAKE_LOCK. Fix: PARTIAL + flag.
53. `src/main/AndroidManifest.xml:7-11` | minor | logic | Missing WiFi-lock, dead storage perm. Fix: add ACCESS_WIFI_STATE, maxSdkVersion.
54. `build.gradle:47-52` | minor | perf | Full-GPL ffmpeg-kit bloat, native path dead. Fix: drop ffmpeg-kit or dead path.
55. `CMakeLists.txt:36-39,53` | minor | logic | file(GLOB) + bad FFmpeg include. Fix: explicit sources, local includes.
56. `src/main/java/com/google/cardboard/codec/CodecSelector.java:20-22` | minor | logic | select() never called. Fix: remove or enforce H264-only.
57. `src/main/java/com/google/cardboard/video/DecoderCapabilityReporter.java:35` | minor | perf | asList.contains per codec. Fix: loop array.
58. `src/main/java/com/google/cardboard/video/VideoDecoder.java:212-224` | minor | logic | May pick SW decoder, unchecked FRAME_DROP. Fix: enumerate HW, guard SDK.
59. `src/main/java/com/google/cardboard/video/VideoDecoder.java:84-89` + `video/VideoWatchdog.java:23-27` | minor | logic | 3s threshold triggers discovery storms. Fix: require 2 stalled polls.
60. `src/main/java/com/google/cardboard/VrActivity.java:388-397` | minor | logic | Camera denial = black screen, no retry. Fix: retry button.
