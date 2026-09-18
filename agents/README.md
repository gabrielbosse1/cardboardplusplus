# Agents — Cardboard++ Performance + Logic Audits

One-shot audit agents, one per component. Each folder holds that agent's full report.

- `app/REPORT.md` — Android client (`cardboardplusplus-android/`): 60 findings (5 critical, 21 major, 34 minor)
- `bridge/REPORT.md` — Desktop Bridge (`bridge/` Rust): 55 findings (6 critical, 19 major, 30 minor)
- `driver/REPORT.md` — SteamVR driver (`driver_cardboardplusplus/` C++): 47 findings (10 critical, 18 major, 19 minor)

Total: 162 findings.

## Top cross-cutting themes

1. Contract drift: `0x12` rotation + sensor port 42074 undocumented in AGENTS.md (flagged by all 3 agents).
2. Logging on hot paths: DebugLog always-on (driver), per-frame LOGD (app JNI), eprintln per 20th packet (bridge).
3. Per-frame allocations: ByteBuffer/DatagramPacket/Runnable per sensor event (app), Vec+clone per camera frame (bridge), malloc per encoded frame (driver).
4. Lifecycle leaks: executor never recreated (app), MediaPipe/ffplay children never killed + SHM leak (bridge), Deactivate never ShutdownBridge (driver).
5. Threading races: unsynchronized VideoDecoder (app), DRIVER_CONN mutex across recv + single Mutex @1kHz (bridge), torn m_serverAddr + non-atomic counters (driver).

Read each REPORT.md for file_path:line_number ranked lists with 1-line fixes.
