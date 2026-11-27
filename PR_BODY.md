## Summary
**Incomplete: do not merge.** This draft is a partial compatibility-audit follow-up, not a completed fix series. The current session branch is fixed by Arena.
Linux Present rejects nonzero synchronization handles until the SteamVR contract is known; affected streams may stop. Imported sync-file descriptors are no longer closed after transfer to Vulkan.
Cap comparisons, failed-copy detection, failed MediaCodec cleanup, Linux layer accumulation and stream toggle were improved.
BT.601 limited codec metadata and a graphics-only queue fallback were added, without hardware validation.

## Disposition (in progress)
| ID | State | Commit | Notes |
|---|---|---|---|
| F01 | pending | — | Not yet fixed or verified. |
| F02 | defensive + diagnostics | ef578e7 | Nonzero sync handles fail closed; no documented contract in bundled headers. |
| F03 | partial | ef578e7 | Consumed FD no longer closed; complete eye synchronization and tests remain. |
| F04 | pending | — | Not yet fixed or verified. |
| F05 | partial | c190d5e | Cap-only updates reach comparison/rebuild; transactional failure rollback remains F06. |
| F06 | pending | — | Not yet fixed or verified. |
| F07 | pending | — | Not yet fixed or verified. |
| F08 | partial | ed07d9c | Vulkan vendor/device/driver logged; adapter matching remains. |
| F09 | pending | — | Not yet fixed or verified. |
| F10 | fixed; runtime unverified | 694c3b9 | Retain up to four layers per frame. |
| F11 | fixed; runtime unverified | 694c3b9 | Honor SHM stream toggle on Linux. |
| F12 | partial | c190d5e | Corrected misleading D3D fallback log; device loss recovery still missing. |
| F13 | pending | — | Not yet fixed or verified. |
| F14 | partial | batch 5 | Requested BT.601 limited codec metadata; Android/VUI verification and HDR handling remain. |
| F15 | pending | — | Not yet fixed or verified. |
| F16 | pending | — | Not yet fixed or verified. |
| F17 | partial | ed07d9c | UI help corrected; named REST codecs still not honored. |
| F18 | pending | — | Not yet fixed or verified. |
| F19 | partial | batch 5 | Skip NV12 compute on graphics-only queue; feature validation remains. |
| F20 | fixed; runtime unverified | c190d5e | Caller requires successful host copy. |
| F21 | fixed; runtime unverified | c190d5e | Release failed MediaCodec initialization. |

## Tests
- `g++ -std=c++17 -fsyntax-only` passed on Linux for DirectModePosix.cpp, EncoderSetup.cpp, VideoEncoderShaders.cpp, VulkanContext.cpp, VideoEncoderFFmpeg.cpp and VulkanCompose.cpp. VulkanCompose used dummy generated SPIR-V declarations for syntax checking only; this is not a shader/build test.
- Standalone driver `test_wire_protocol.cpp` compiled and passed; GCC emitted existing `-Wstringop-overflow` warnings in the test code.
- `git diff --check` passed. No full Linux link/build, Windows build, Android/Rust/Slint build, fault injection, graphics validation, SteamVR, phone or hardware tests were run; CMake, Cargo/Rust, Android SDK, DirectX Shader Compiler, Windows compiler and required Linux runtime/development dependencies were unavailable. See [hardware checklist](audit-reports/hardware-test-checklist.md).

## Protocol changes
No control message, UDP framing or SHM layout changes. F14 requests limited-range SMPTE 170M/BT.601 H.264 color metadata in encoder output, which may change in-band bitstream metadata. Encoder acceptance and old phone/preview behavior are unverified.

## UI changes
Encoder help now lists the platform-specific hardware candidates and software fallback; controls unchanged. Rendering unverified.

## Open questions / decisions taken — blocking
- Linux `Present` synchronization contract is unspecified by the bundled OpenVR header. Nonzero sync handles now fail closed; this can stop affected streams. Determine handle type and ownership before enabling the path.
- The requested branch name cannot be created in this session; the Arena branch is fixed.
- The requested shared structured-logging utility, VAAPI frames/upload and first-frame fallback, lifecycle safety, recovery/status protocol, per-configuration capability probe, and several compatibility items remain incomplete. They are not silently treated as terminal.
- BT.601 limited was selected to match the existing shader; encoder signaling and phone interpretation need color-bar verification. HDR rejection/tone-mapping is not implemented.
- Findings classified as disputed: none. See worklog for each re-verification and outstanding work.

## Rollback
Revert batch 5 to undo NV12 queue gating, H.264 color metadata, and checklist/worklog changes.
Revert ed07d9c to undo Vulkan identity logging and the encoder help text.
Revert 694c3b9 to undo Linux layer/toggle changes.
Revert c190d5e to undo the cap, copy-status and decoder-lifetime fixes.
Revert ef578e7 to restore the original sync behavior (which includes invalid semaphore operations and potential FD double-close).
