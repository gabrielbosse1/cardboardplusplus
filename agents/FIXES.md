# Fixes — criticals from the perf+logic audits

Baseline: `baa67df` (master). All work on git branches, nothing pushed.

## Branches

- `fix/app-criticals-clean` — 5 commits, android files only
  `3140767` start-code `00 00 00 01` · `ed6b050` VideoManager restart on
  resume · `e9a6a39` recreate telemetry executor · `471add6` sync
  updateVideoTexture · `9e58959` delete GL texture on GL thread
- `fix/bridge-criticals-clean` — 7 commits, bridge files only
  `2d7eafc` MSAA staging+resolve · `523d2ed` SHM unlink owned region ·
  `634365f` MediaPipe stdout null · `6365257` store Child, kill on
  shutdown · `9b2b444` mediapipe reconnect+backoff · `6530388` atomic
  driver install · `a3a3a98` build fix (clone core for poller, drop dead resolve)
- `fix/driver-criticals` — 7 commits, driver files only
  `981183f` honor preview flag · `86426db` DebugLog gated ·
  `b30b047` drop per-GetPose log · `8371687` honor GPU/SW request ·
  `ea7903a` no zeroed telemetry · `2b1a08e` ShutdownBridge+free all
  pids · `e5d1960` zero swap out-param
- `fix/all-criticals` — merges all three. This is the tested integration tip.

Note: parallel agents briefly cross-committed to each other's branches.
Cleaned by cherry-picking app-only / bridge-only commits into the `-clean`
branches above; contaminated originals deleted. Driver branch was clean.

## Joint test results (on `fix/all-criticals`)

- Bridge `cargo test`: 135 passed, 0 failed
  (53 lib + 58 bin + 11 mock_driver + 13 mock_phone).
- `git diff --check`: clean. `CardboardWire.h`: untouched (wire contract intact).
- Android unit tests: NOT run — no Java/Gradle in this environment
  (fixes verified by re-reading edited regions only).
- Driver compile: NOT run — no msbuild here (tests are post-build events).

## Deliberately skipped (need hardware)

- Driver pose quat `qDriverFromHeadRotation` vs sensor quat — changes
  SteamVR tracking, needs on-headset validation.
- Controller sine-bob → bridge hand data — gameplay change, needs new
  plumbing, not a minimal fix.
