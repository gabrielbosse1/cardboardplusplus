# Compatibility fix worklog

Branch: `arena/1ac5e66b-cardboardplusplus`; base: `51fca6af3cf174db4c24e16a91a4b7f9a31f770`.
Current batch: 1 (partial). Next action: draft PR and implement independently verifiable fixes; VAAPI and complete eye synchronization remain open.

| ID | Verdict | Status | Commit | Remaining work |
|---|---|---|---|---|
| F01 | Confirmed: VideoEncoderFFmpeg::InitializeFFmpeg configures VAAPI without a hardware frames pool. | Pending | — | Implement upload and first-frame fallback. |
| F02 | Confirmed: DirectModePosix::AcquireSyncTexture passed an import structure to semaphore creation and waited on a binary semaphore. Bundled openvr_driver.h does not specify the handle contract. | Defensive+diagnostics | Batch 1 | Nonzero sync handles are dropped until their type and ownership are established. |
| F03 | Confirmed: VulkanCompose::ImportEyeSemaphores closed consumed FDs after successful import. | Partial | Batch 1 | Require synchronization of every eye and test submit-failure cleanup. |
| F04 | Not yet reverified | Pending | — | Reverify and disposition |
| F05 | Not yet reverified | Pending | — | Reverify and disposition |
| F06 | Not yet reverified | Pending | — | Reverify and disposition |
| F07 | Not yet reverified | Pending | — | Reverify and disposition |
| F08 | Not yet reverified | Pending | — | Reverify and disposition |
| F09 | Not yet reverified | Pending | — | Reverify and disposition |
| F10 | Not yet reverified | Pending | — | Reverify and disposition |
| F11 | Not yet reverified | Pending | — | Reverify and disposition |
| F12 | Not yet reverified | Pending | — | Reverify and disposition |
| F13 | Not yet reverified | Pending | — | Reverify and disposition |
| F14 | Not yet reverified | Pending | — | Reverify and disposition |
| F15 | Not yet reverified | Pending | — | Reverify and disposition |
| F16 | Not yet reverified | Pending | — | Reverify and disposition |
| F17 | Not yet reverified | Pending | — | Reverify and disposition |
| F18 | Not yet reverified | Pending | — | Reverify and disposition |
| F19 | Not yet reverified | Pending | — | Reverify and disposition |
| F20 | Not yet reverified | Pending | — | Reverify and disposition |
| F21 | Not yet reverified | Pending | — | Reverify and disposition |
