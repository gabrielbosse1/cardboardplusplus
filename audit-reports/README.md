# Audit Reports — 2026-09-19

Report-only audits. No code changed.

- `driver-audit.md` — SteamVR driver (`driver_cardboardplusplus/`)
- `bridge-audit.md` — Bridge (`bridge/`)
- `android-audit.md` — Android app (`cardboardplusplus-android/`)
- `cross-component-audit.md` — cross-component + data flow (ports 42069-42074, 8567, SHM)

Each file lists: file:line, category, severity (high/med/low), evidence, why it matters, fix suggestion.

Top cross-cutting themes from all 4 agents:
1. Driver `GetPose` writes phone quat to legacy field only, live field stays identity + sine-bob on top of real tracking.
2. Driver telemetry callback never invoked + re-init drops it → SHM telemetry dead; `m_streamEnabled` never read → Bridge on/off is no-op.
3. UDP video 60KB datagrams → guaranteed fragmentation; smaller chunks (~1400B) needed.
4. FFmpeg runtime DLLs missing from repo + installer silently skips → driver fails to load on fresh checkout.
5. Bridge `POST /settings` partial body resets to compiled defaults; STATS handling stops drain + only ACK refreshes liveness → UI flaps.
6. Bridge ships exe only — sidecar .py, model, ffmpeg, python pins missing.
7. Android dead FFmpeg software decoder (~470 lines) justifying full-gpl ffmpeg-kit dep; keyframe scan 3-4x per frame; telemetry hot-path allocs on UI thread.
8. No version negotiation despite `v1` strings; wire constants triplicated in 8+ files; timeout ladder disagrees (2s vs 4s vs 5s).
9. Phone discovery broadcasts forever even after ACK; watchdog reconnect is no-op.
10. Hand pipeline can be silently absent while stream looks fine (violates ship-together rule).
