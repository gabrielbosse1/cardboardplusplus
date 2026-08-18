# SteamVR Driver — Flicker (App ↔ Black) & Frozen-UI Issue Map

**Status:** Diagnostic map (not yet a fix).
**Author:** Generated with Hy3 during triage.
**Date:** 2026-08-18
**Scope:** `driver_cardboardplusplus/src/HmdDriver.cpp`, `.../src/VideoEncoder.cpp`, `.../include/HmdDriver.h`.

---

## 0. TL;DR (the short version)

**CONFIRMED 2026-08-18 — logs prove H1 is the root cause.** The driver stores
only the **last** `SubmitLayer` call in `m_submitLayers` (a single two-eye pair)
and `Present` composites exactly that one pair. In an app, SteamVR submits
**multiple layers** (scene + overlay, often a separate swap set each) interleaved
with the Home/dashboard layer. `m_submitLayers` then holds whichever layer was
submitted *last* before each `Present` — a random mix of Home / app-scene /
app-overlay every frame → **flicker**. When that last layer is an
empty/unrendered slot → **black frame**. Overlays (dashboard, desktop, 2D dialogs)
never get composited, which is why the app UI is unusable.

Earlier §8 downgrade (H5/H6) is **overturned**: logs show the eye format is
identical home vs app (`0x1d` = R8G8B8A8_UNORM_SRGB, 8-bit) and the fixed-slot
experiment (Test B) did **not** stop the flicker. See **§8 (re-revised)** and
**§10 (log evidence)**.

1. **Flicker app ↔ black** — `m_submitLayers` alternates between the real scene
   layer (good) and an overlay/empty layer (black) because only the *last*
   submitted layer survives.
2. **"Apps stop behaving / can't use the desktop window / can't use the app UI"**
   — dashboard, desktop, and in-app 2D overlays are never composited (driver
   keeps only one layer), so they are missing/broken in the only display this
   HMD has (the streamed phone screen).

---

## 1. Symptoms as reported

- Screen flickers between the current app frame and a pitch-black frame as soon
  as an app is open.
- The app (and SteamVR environment) "stops behaving":
  - SteamVR **Media Player** is not flagged "Not Responding" by Windows, but is
    stuck / unusable.
  - The **Desktop window** panel cannot be used.
  - In the SteamVR **tutorial** (the white robot / blue eye intro), the
    **sound-test dialog** (the "is your stereo working?" step) UI cannot be
    used at all.
- Previous mitigations already applied (and which did NOT fix it):
  - Real triple buffering (3 distinct shared textures per swap set).
  - Safe create/destroy of swap texture sets (`WaitEncoderIdle`, `m_sceneTearingDown`).

---

## 2. Architecture recap (data flow)

```
VR app / SteamVR compositor
   │  per frame, per LAYER:
   │    GetNextSwapTextureSetIndex(...)      → driver picks eye slot
   │    SubmitLayer( perEye[2] )             → driver receives eye texture handles
   │    Present( syncTexture )               → driver "displays" (here: streams)
   ▼
HmdDriver::SubmitLayer   (stores ONLY last two-eye pair in m_submitLayers[0],[1])
HmdDriver::Present       (acquires syncTexture mutex, copies eye textures →
                          private shared copies, queues ONE frame)
   ▼  (background thread)
HmdDriver::EncodingThreadFunc → EncodePendingFrame
   │  opens private copies on a 2nd D3D11 device
   │  ComposeSBSGPU(left,right)   → draws SBS into m_pConversionRT
   │  ReadbackToBuffer()          → GPU→CPU staging map (stall)
   │  SwsConvert()                → BGRA→NV12 on CPU
   │  FinishEncode()              → H264 encode + Annex-B BSF + UDP send
   ▼
Phone (Cardboard) decoder → display
```

**Key OpenVR fact (verified against Valve docs + openvr issues #359/#358/#351):**
- `Present(syncTexture)`'s `syncTexture` is a **1×1 dummy texture used only as a
  lock** ("it has a lock for the shared textures"). You must `AcquireSync`/
  `ReleaseSync` it (this driver does — `AcquireSyncTexture`, `HmdDriver.cpp:596`).
  You do **NOT** read pixels from it. Your code is correct here.
- `SubmitLayer` is **called once per layer**, not once per frame. The driver
  owns compositing. This is the loaded gun.

---

## 3. Root-cause hypotheses (ranked)

### H1 — Driver keeps only the last layer; overlays overwrite the scene (PRIMARY)
**Confidence:** High. **Severity:** Critical. **Matches BOTH symptoms.**

Evidence:
- `SubmitLayer` stores a single two-eye pair and overwrites it on every call:
  - `HmdDriver.cpp:469-479` — `m_submitLayers[0].hTexture = perEye[0].hTexture;`
    `m_submitLayers[1].hTexture = perEye[1].hTexture; m_hasSubmit = true;`
  - `m_submitLayers[2]` declared as a single pair only: `HmdDriver.h:163`.
- `Present` then composites exactly that one pair:
  - `HmdDriver.cpp:524-528` (map lookup) and `:566-569` (copy).
- `ComposeSBSGPU`/SBS shader assumes the submitted textures are the full
  left/right eye scene at swap-set size:
  - `VideoEncoder.cpp:92-104` (uv.x<0.5 → left eye, else right eye).

Consequence when an overlay (dashboard / desktop / keyboard / 2D dialog) is
submitted as a separate `SubmitLayer`:
- `m_submitLayers` is replaced by the overlay's texture pair.
- The overlay's texture is typically a different size, possibly a single full
  frame (not per-eye), and may be largely empty/transparent in its render target.
- The SBS shader then "composites" the overlay as if it were the eye scene →
  black/garbage, or a frozen overlay image.
- Because overlays appear/disappear as you interact, the streamed image
  **flickers between the real scene and the black/broken overlay frame**.

Why this also explains the "frozen UI":
- The dashboard / desktop / in-app 2D UI are exactly the things submitted as
  extra layers. Since the driver never composites them (and they clobber the
  scene), the only display this HMD has (the phone stream) shows no usable
  dashboard/desktop/2D-UI. The tutorial sound-test dialog is a 2D overlay →
  "can't use the app UI at all."

Why your previous fixes didn't help:
- Triple buffering and safe create/destroy fix **texture lifecycle races**.
  The bug is one level up: you discard all but the last layer every frame.

### H2 — Render-target / recommended-size mismatch causes mis-scaled or empty regions (SECONDARY)
**Confidence:** Medium. **Severity:** Moderate.

Evidence:
- `GetRecommendedRenderTargetSize` returns **960×1080 per eye** (`HmdDriver.cpp:271-275`)
  while the encoder reads an SBS frame of **2880×1620** (`m_encoderW/H`,
  `HmdDriver.cpp:803-804`). `GetEyeOutputViewport` returns `1920/2 × 1080`.
- SteamVR may allocate swap textures at sizes that don't match 1440×1620 per
  eye (the GitHub thread #409 notes sizes like 2048×1024 and 13xx×14xx appear at
  runtime). The SBS shader remaps by UV so it still draws, but mismatches can
  leave letterboxed/black borders or uneven eye sizing.
- This alone would not produce pure-black flicker, but it widens black borders
  and can make a clobbered layer look fully black.

### H3 — OpenVR "right eye always index 0" quirk (KNOWN, low impact here)
**Confidence:** High that the quirk exists; Low that it causes *this* flicker.

Evidence / background:
- openvr issue #359: SteamVR ignores the index returned by
  `GetNextSwapTextureSetIndex` for the **right** eye and always uses slot 0.
- This driver round-robins both eyes (`HmdDriver.cpp:452-467`). Because
  `Present` uses the *actually submitted* handles (`m_submitLayers`), the
  right-eye handle is still correct; the round-robin advancement for the right
  set just drifts harmlessly. Not a black-frame source, but worth noting the
  right-eye triple buffer is effectively unused (only slot 0 matters).

### H4 — Encoder/CPU stall freezes the streamed frame (possible contributor to "stuck")
**Confidence:** Medium. **Severity:** Moderate.

Evidence:
- `ReadbackToBuffer` does `Map()` on a `D3D11_USAGE_STAGING` texture
  (`VideoEncoder.cpp:820-844`) — an implicit GPU→CPU pipeline stall every frame.
- `SwsConvert` does BGRA→NV12 on the CPU (`VideoEncoder.cpp:1099-1125`) at
  2880×1620 — several ms/frame.
- The background thread is gated by `m_encodeDone`, so a slow frame makes
  `Present` **drop** subsequent frames (`HmdDriver.cpp:542-552`) — the phone
  would freeze on the last frame, not flicker to black. This can add to the
  "stuck" feeling but is not the primary flicker cause.

---

## 4. Why "black" specifically (not just dropped frames)

When `Present` bails out early (no submit / encoder busy / map fail / sync
fail), it simply **drops** the frame — the phone keeps showing the last good
frame. Dropped frames = *freeze*, not *black flicker*.

To get an actual **black frame in the stream**, the encoder must encode a
black/empty image. The only path that produces that is `ComposeSBSGPU` being fed
an empty/overlay/wrong-sized eye texture — i.e. **H1** (wrong layer captured).
This is the strongest argument that H1 is the real cause.

---

## 5. Diagnostics to confirm H1 (low effort, high signal)

Add temporary logging and watch `vrserver`/`driver` logs:

1. **Count SubmitLayer calls per Present.**
   - In `SubmitLayer` (`HmdDriver.cpp:469`) increment a per-frame counter; in
     `Present` (`HmdDriver.cpp:481`) log it and reset.
   - Expected: `1` for a bare app, `>=2` whenever an overlay/dashboard/2D-UI is
     visible. If you see `2+` exactly when the flicker starts → H1 confirmed.

2. **Log the size + handle of each submitted layer.**
   - Print `perEye[0].hTexture`, `perEye[1].hTexture` and the texture
     `D3D11_TEXTURE2D_DESC` (width/height/format) per `SubmitLayer`.
   - If the *second* layer per frame has a different size than the first (or is
     the same handle repeated), that's the overlay clobbering the scene.

3. **Trigger test:** open an app → no flicker in home/dashboard. Open the
   **Desktop panel** or the **tutorial sound-test dialog** → flicker begins and
   SubmitLayer count jumps. If yes → H1.

4. **Sanity:** confirm `AcquireSync` rarely fails (`HmdDriver.cpp:624-628`). If
   it fails a lot, the stream would *freeze*, not flicker — different branch.

---

## 6. Fix directions (for after confirmation)

**Primary fix (H1): composite all layers, not just the last.**
- Replace the single `m_submitLayers[2]` with a `std::vector<SubmitLayerInfo>`
  accumulated across all `SubmitLayer` calls between two `Present`s.
- In `Present`, after acquiring sync, compose layers in submission order
  (painter's algorithm) into the SBS output: draw the scene first, then each
  overlay on top, honoring `bounds` (`VRTextureBounds_t`) and per-eye eye
  texture, with alpha blending enabled in the SBS pipeline
  (`VideoEncoder.cpp:1021` `ComposeSBSGPU` currently clears + overwrites, no
  blend across layers).
- Track which layer is the "scene" (usually the first / largest / flagged
  `Submit_TextureWithPose`) vs overlay, or simply composite in order with blend.

**Secondary (H2): align sizes.**
- Make `GetRecommendedRenderTargetSize` and `GetEyeOutputViewport` match the
  encoder's per-eye resolution (`m_encoderW/2`, `m_encoderH`), and let the SBS
  shader scale any incoming size (already UV-based, so mostly free).

**Tertiary (H4): reduce stall.**
- Move BGRA→NV12 to the GPU (compute shader / D3D11 video processor) and/or
  avoid the per-frame `Map` stall by double-buffering the readback. This reduces
  "stuck" feel and dropped frames but is not the flicker root cause.

---

## 7. Open questions for you (need answers to finalize the fix)

1. **Which screen flickers** — the streamed phone view, the SteamVR "VR Mirror"
   window on your desktop, or the actual headset output (if any)?
2. **Does the flicker start exactly when you open the Dashboard / Desktop panel
   / any 2D dialog?** (This is the H1 trigger; if yes, we're 90% there.)
3. Are you running with a real HMD attached, or is this driver the *only*
   display (pure stream-to-phone)? That tells us whether the "frozen UI" is on
   the stream or in SteamVR itself.
4. Do you want me to (a) just confirm H1 with the logging above, or (b) go ahead
   and implement the multi-layer compositing fix once confirmed?

---

## 8. REVISION — app-only flicker (RE-REVISED after log analysis)

**New facts from user (2026-08-18):**
- Flicker = app ↔ black. Seen on **SteamVR mirror AND Cardboard/phone** (no real HMD).
- Reproduces in a **pure app** (single scene layer), e.g. SteamVR Tutorial, Media Player.
- Does **NOT** happen in SteamVR Home / system menu / dashboard.
- Both displays flicker together → the source of black is the **per-eye textures
  the driver reads**, not the encoder pipeline (phone and mirror both derive from
  the same driver read).

**CORRECTION (post-logs):** the "pure app, single layer" assumption was wrong.
Even the Media Player creates **two** swap sets (scene + overlay) for its pid, and
`SubmitLayer` is interleaved across Home + app layers. So this IS the H1
single-layer-clobber bug, just triggered by the app's own extra layer rather than
the dashboard. H1 is therefore the **primary** cause again.

### H5 / H6 — RULED OUT by the logs (2026-08-18 test session)
- **H6 (format):** every `[DIAG compose]`/`[DIAG Present]` shows `Lfmt=Rfmt=0x1d`
  (R8G8B8A8_UNORM_SRGB, 8-bit) in **both** Home (956×1076) and the app (1152×1296).
  No HDR/10-bit/linear format appears → H6 discarded.
- **H5 (swap-slot timing / round-robin):** Test B (fixed slot 0 for both eyes)
  did **not** change the flicker. Also `Present` already reads the *submitted*
  handles, not `nextIndex`, so the round-robin is not the source. H5 discarded as
  the primary cause (the round-robin is fine).

### Revised hypothesis ranking

#### H5 (NEW LEAD) — Driver reads an un-rendered/cleared swap slot on alternating frames (apps only)
**Confidence:** High that *something* like this is happening; mechanism TBD.
**Severity:** Critical. Matches "app↔black, not home, both displays."

Why apps and not home:
- In Home/dashboard the present path is simple and steady; the driver's
  `m_encodeDone` gate + single-in-flight frame stay in sync.
- In an app, SteamVR drives the swap set at full rate and may re-present /
  reproject, so the slot the app just rendered and the slot the driver copies can
  diverge on some frames → driver copies a slot the app hasn't filled this frame
  (black/cleared) → **black frame**, then next frame the correct slot → **app
  frame** → flicker.

Likely contributors in this code:
- `GetNextSwapTextureSetIndex` round-robins **both** eyes
  (`HmdDriver.cpp:452-467`). OpenVR bug #359 says SteamVR **ignores the returned
  index for the RIGHT eye and always uses slot 0**. The driver advances the right
  set's `nextIndex` anyway, so the right-eye set's internal counter drifts vs what
  SteamVR actually renders. Even though `Present` reads the *submitted* handle
  (`m_submitLayers`), any path that trusts `nextIndex` instead of the submitted
  handle is a desync risk. **Verify the driver NEVER derives the copied texture
  from `nextIndex`** — currently it uses submitted handles, which is correct, so
  the desync must come from elsewhere (timing, not index).
- The copy in `Present` (`HmdDriver.cpp:566-569`) reads `pLeftTex`/`pRightTex`
  after `AcquireSync` on the syncTexture. Per OpenVR docs that *should* guarantee
  the submitted textures are ready. If `AcquireSync` sometimes succeeds but the
  app is still mid-render into that slot (e.g. app reuses a slot the driver thinks
  is free), you get a partially/never-rendered (black) read.

#### H6 (NEW) — Per-eye texture FORMAT differs in apps vs Home
**Confidence:** Medium. **Severity:** High (can produce black/wrong).

- Home/dashboard may submit **BGRA8 (8-bit)** eye textures; a real app very often
  submits **R10G10B10A2 (10-bit)** or **R16G16B16A16_FLOAT (HDR/scRGB)**.
- The swap textures are created in the app's format (`HmdDriver.cpp:331`) and the
  private copies match (`HmdDriver.cpp:659`), but the SBS compose path builds the
  SRV from `leftDesc.Format` (`VideoEncoder.cpp:944`) and the shader treats it as
  `float4`. A 10/16-bit linear value run through `linearToSrgb` can be very dark;
  an unexpected/unsupported format makes `CreateShaderResourceView` fail →
  `ComposeSBSGPU` returns false → no frame (freeze) **or** black.
- This would be app-only (apps use HDR formats; Home may not) → consistent with
  the report.

#### H7 (NEW) — No "last-good-frame" guard, so any empty/black read becomes a black stream frame
**Confidence:** High (design gap). **Severity:** Moderate but multiplies the pain.

- Whenever `ComposeSBSGPU`/`Readback` yields black (H5/H6), the encoder still
  emits that black frame. There is no fallback to re-send the previous good frame.
- Adding a guard (if the composed frame is all-zero / compose failed → keep last
  encoded frame instead of sending black) would turn "app↔black flicker" into
  "app↔frozen last frame," which is far less nauseating and confirms H5/H6 is the
  source.

### Revised diagnostics (do these to pin H5/H6)

1. **Log per Present:** submitted left/right handles, the slot `nextIndex` the
   driver *thinks* is current, the eye texture `D3D11_TEXTURE2D_DESC.Format`
   (BGRA8 vs R10G10B10A2 vs float), and whether `ComposeSBSGPU` succeeded.
2. **Detect black frames in the encoder:** after `ReadbackToBuffer`, scan a
   downsampled region of `m_pReadbackBuffer`; if mean ≈ 0 → log
   `"BLACK FRAME: pid=... fmt=... slot=... AcquireSyncOK=..."`. Correlate black
   frames with format + slot to decide H5 vs H6.
3. **Toggle test:** temporarily make `GetNextSwapTextureSetIndex` return a FIXED
   index (e.g. always 0 for both eyes) instead of round-robin. If flicker stops,
   the round-robin timing is the trigger (H5). If it persists, it's format (H6).
4. **Home vs app format check:** compare logged `Format` in Home vs in the
   Tutorial — if they differ, H6 is confirmed.

### Revised fix directions

- **H5:** keep reading the *submitted* handles (already correct), but stop
  round-robin drift for the right eye (the #359 quirk), and ensure `Present`
  copies only after `AcquireSync` truly owns the frame; consider presenting on a
  fixed slot or honoring SteamVR's actual submitted slot precisely.
- **H6:** make the SBS compose path format-aware — detect R10G10B10A2 / float and
  convert to BGRA8 correctly (the existing `ConvertViaShader` path already exists
  for non-BGRA; ensure it is taken for HDR formats instead of a straight SRV
  sample that darkens/blacks).
- **H7:** add a last-good-frame guard so a failed/black compose does not emit
  black to the encoder.

> H1 (overlay/clobber) is still real and should be fixed for dashboard/desktop/2D-UI
> support (§6), but it is **not** what causes the app↔black flicker you're seeing
> right now.

---

## 9. Next step (ask)

Do you want me to:
- **(A)** add the §8 diagnostics (black-frame detection + per-Present format/slot
  logging) so we can confirm H5 vs H6 from logs, or
- **(B)** directly try the cheap experiment in §8-diag-3 (disable round-robin,
  fixed slot) to see if flicker disappears, or
- **(C)** implement the H7 last-good-frame guard now (safe, reduces nausea
  immediately) and then continue diagnosing?

---

## 10. CONFIRMED — log evidence from 2026-08-18 test session

Session: `C:\Program Files (x86)\Steam\logs\vrserver.txt`
(13:33:34 Activate → 13:34:03 app open → 13:34:10 app close → 13:34:14 SteamVR off).
Driver built with Test A diagnostics (`SubmitLayer`/`[DIAG Present]`/`[DIAG compose]`/
`[DIAG BLACK FRAME]`).

### What the logs prove
1. **Only ONE swap set per eye in Home** (956×1076, fmt=29/0x1d):
   - Left set: 3221230914 / 3221232258 / 2147494402 (slots 0/1/2)
   - Right set: 214975554 / 1073750914 / 1073749442
   → single layer → no flicker in Home (matches report).
2. **App (`steamvr_media_player`, pid=5544) creates TWO swap sets**
   (both 1152×1296, fmt=0x1d) at 13:34:03.262 / .338:
   - app set 1: 2147489794 / 3221227330 / 2147494082
   - app set 2: 3221229122 / 2147490562 / 1073753602
   → **scene + overlay = two layers** (the "pure app" assumption was wrong).
3. **`SubmitLayer` is interleaved across Home + both app sets** once the app opens
   (e.g. `...left: 2147494082,right: 1073753602` (app set1+set2) immediately
   followed by `...left: 3221230914,right: 2147495554` (Home)). `Present` then
   composites whatever `m_submitLayers` currently holds = the **last** submitted
   layer → random Home/app/overlay mix → flicker.
4. **Black frames appear exactly at app-open** (`[DIAG BLACK FRAME] meanRGB=3` @
   13:34:03.815 and `meanRGB=0` @ 13:34:03.945) — i.e. when the last-submitted
   layer before a `Present` was an empty/black slot (overlay not yet rendered, or
   Home layer hidden). They stop once the app's own layer dominates, but the
   Home/app/overlay *content* flicker continues for the whole app session.
5. **Format identical home vs app** (`0x1d` sRGB 8-bit) → H6 discarded.
6. **Test B (fixed slot) did not fix flicker** → H5 discarded as primary.

### Confirmed root cause
`HmdDriver::SubmitLayer` overwrites a **single** two-eye pair in `m_submitLayers`;
`Present` + `ComposeSBSGPU` stream that one pair. With multiple layers (Home +
app-scene + app-overlay) submitted per frame, the driver shows only the *last*
one → app↔black flicker and broken/missing UI. **This is H1, confirmed.**

### Fix (now the only one that matters)
Composite **all** `SubmitLayer` calls per frame, in submission order (painter's
algorithm), instead of keeping only the last. Concretely:
- Replace `m_submitLayers[2]` (single pair) with a
  `std::vector<SubmitLayerInfo>` accumulated across every `SubmitLayer` between
  two `Present`s.
- In `Present`, after `AcquireSync`, drive `ComposeSBSGPU` to draw each layer in
  order (scene first, overlays on top) with alpha blending, honoring each layer's
  `bounds`/eye texture. Currently `ComposeSBSGPU` clears + overwrites a single
  pair (no multi-layer, no blend) — needs to become a loop with a blend state.
- Then delete the now-irrelevant Test A diagnostics (or keep them behind a flag).

This simultaneously fixes the flicker AND the unusable dashboard/desktop/2D-UI.
