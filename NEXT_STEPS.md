# Next Steps — Flicker Fix Plan (A + B)

**Derived from:** `FLICKER_ISSUE_MAP.md` §8.
**Goal:** confirm the root cause of the app↔black flicker (H5 slot/timing vs H6 format).
**Excluded:** option C (last-good-frame guard) — deliberately dropped; re-sending a stale
frame would mask the bug and make diagnosis harder, and could hide real regressions.

---

## Test B — Fixed-slot diagnostic (DO THIS FIRST)

**What it is:** `GetNextSwapTextureSetIndex` (HmdDriver.cpp:452) now returns a FIXED
slot (0) for both eyes when `DRIVER_DIAG_FIXED_SLOT` is `1`, disabling the round-robin.
This tests hypothesis H5: that the per-eye slot the driver returns desyncs from the
slot the app actually renders into (OpenVR bug #359 — SteamVR ignores the returned
index for the right eye and always uses slot 0; we now also fix the left eye).

**Build & install:**
1. Rebuild `driver_cardboardplusplus` (Release, x64).
2. Copy the new `driver_cardboardplusplus.dll` into
   `SteamVR\drivers\cardboardplusplus\bin\win64\`.
3. Restart SteamVR.

**Interpret:**
- **Flicker stops** → H5 confirmed. The returned slot was the mismatch. Real fix:
  always hand back the slot SteamVR actually renders (slot 0 / the slot it submits),
  not a round-robin index. Keep `DRIVER_DIAG_FIXED_SLOT` behaviour as the fix.
- **Flicker continues** → H5 ruled out for this mechanism. Proceed to test A.

**Revert:** set `DRIVER_DIAG_FIXED_SLOT` to `0` and rebuild.

---

## Test A — Black-frame + format/slot logging (DO AFTER B if B fails)

**What it is:** add instrumentation so logs conclusively show whether black frames
come from a wrong slot (H5) or a wrong texture format (H6).

**Changes to make:**
1. In `Present` (HmdDriver.cpp:481), log per call: submitted left/right handles,
   the `nextIndex` the driver currently thinks is active, and the result of
   `AcquireSync`.
2. In `EncodePendingFrame` / `ComposeSBSGPU`, log the eye textures'
   `D3D11_TEXTURE2D_DESC.Format` (BGRA8 vs R10G10B10A2 vs float) for Home vs app.
3. In `VideoEncoder::ReadbackToBuffer` (VideoEncoder.cpp:810), after the copy,
   scan a downsampled region of `m_pReadbackBuffer`; if mean ≈ 0, log
   `"BLACK FRAME: fmt=... slot=... AcquireOK=..."`.

**Interpret:**
- Black frames correlate with a specific **format** (e.g. R10G10B10A2 in apps,
  BGRA8 in Home) → H6 confirmed. Fix: make the SBS compose path format-aware
  (route HDR formats through the existing `ConvertViaShader` path instead of a
  straight SRV sample that darkens/blacks).
- Black frames correlate with a specific **slot index** while format is constant →
  H5 (timing) variant. Fix per slot handling.

---

## Order
1. Run Test B. Report result.
2. If B fails → implement and run Test A. Report logs.
3. Apply the real fix indicated by B/A.
4. (Later, separately) Fix the latent H1 overlay-clobber bug from §6 so dashboard /
   desktop / 2D-UI work — but that is NOT the current flicker.
