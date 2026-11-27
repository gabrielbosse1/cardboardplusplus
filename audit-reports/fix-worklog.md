# Compatibility fix worklog

Branch: `arena/1b11b2be-cardboardplusplus`; base: `51fca6af3cf174db4c24e16a91a4b7f9a31f770`.
Regression review and cross-platform verification: `audit-reports/branch-regression-report-2026-11-27.md`.

| ID | Verdict | Status | Commit | Remaining work |
|---|---|---|---|---|
| F01 | Confirmed: VideoEncoderFFmpeg::InitializeFFmpeg configures VAAPI without a hardware frames pool. | Pending | — | Implement upload and first-frame fallback. |
| F02 | Confirmed: DirectModePosix::AcquireSyncTexture passed an import structure to semaphore creation and waited on a binary semaphore. | Fixed | fa6c3e1 | Non-zero IPC sync handles log once and rely on per-eye dma-buf fences in VulkanCompose. |
| F03 | Confirmed: VulkanCompose::ImportEyeSemaphores closed consumed FDs after successful import. | Fixed | ef578e7, fa6c3e1 | All submitted layers wait at fragment-shader stage; semaphores cleaned up on submit failure. |
| F04 | Confirmed: Present queues raw FDs and DestroySwapTextureSet can close them while encoding. | Pending | — | Implement lifetime-safe completion and teardown. |
| F05 | Confirmed: ApplyHardwareCap mutated active dimensions before change comparison. | Partial | c190d5e | Verify cap-only reinit on hardware and failure rollback. |
| F06 | Confirmed: Activate tolerates encoder failure and reinit discards old encoder. | Pending | — | Transactional first-frame rollback and retry. |
| F07 | Confirmed: fallback occurs on open only; frame errors do not trigger recovery. | Pending | — | Probe and recover safely. |
| F08 | Confirmed: no explicit compositor/encoder adapter matching; Linux selects first Vulkan device and renderD128. | Partial diagnostics | ed07d9c | Match real compositor and encoder identities. |
| F09 | Confirmed: bridge infers encoder active from ACK and requested name from settings. | Pending | — | Versioned actual encoder status and backward compatibility. |
| F10 | Confirmed: SubmitLayer cleared previous submitted layers. | Fixed | 694c3b9, a1a452e | SubmitLayerQueue drains/discards staged layers on every Present; unit-tested in test_wire_protocol.cpp. |
| F11 | Confirmed: Linux Present ignored stream-enabled flag. | Fixed, runtime unverified | 694c3b9 | Test toggle through both settings interfaces. |
| F12 | Confirmed: device-lost recovery absent; stale D3D fallback message. | Partial | c190d5e | Device-loss classification and recovery still required. |
| F13 | Risk reverified: capOf returns independent max dimensions, while createDecoderByType can select another decoder. | Pending | — | Probe actual size/rate and selected decoder, coordinate negotiation. |
| F14 | Risk reverified: shader uses BT.601 limited without explicit codec metadata; HDR is not preserved. | Partial | 329c3b9 | Test VUI on every encoder and reject or tone-map HDR safely. |
| F15 | Risk reverified: VAAPI device is hard-coded renderD128. | Pending | — | Select a permitted matching device and log access failures. |
| F16 | Confirmed: CreateSharedEye and swap-set failure paths leak partially created resources. | Pending | — | Transactional allocation cleanup and fault injection. |
| F17 | Confirmed: named codecs are reduced to a GPU boolean by ApplyBridgeCfg. | Partial | ed07d9c | Reject or honor explicit REST names; UI hint corrected. |
| F18 | Confirmed: preview FFmpeg stderr is discarded and logging suppressed. | Pending | — | Capture bounded errors and child exit status. |
| F19 | Risk reverified: graphics-only queue may be selected for compute. | Partial | 329c3b9 | Validate all required device features and old driver behavior. |
| F20 | Confirmed: CopyToHost exposed old mapped data after failure. | Fixed, untested at runtime | c190d5e | Fault-inject all copy stages. |
| F21 | Confirmed: configureLocked drops created codec reference on exceptions. | Fixed, untested at runtime | c190d5e | Test repeated Android configure/start failures. |
