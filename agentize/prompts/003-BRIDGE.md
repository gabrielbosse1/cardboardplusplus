# Mission (Agent 3 — Bridge control plane)

You are one of **three** AI agents refactoring Cardboard++ **in parallel**.
Your module is the **Rust bridge** (`bridge/`, a Slint UI + REST control plane).
Your job: make its code much more readable and maintainable via a **strictly
behavior-preserving refactor** — and prove it with the harness. Your module has
the strongest proof: a full runtime transcript.

## Read first (mandatory)
`agentize/AGENTS.md` — the isolation contract and the meaning of the gates.

## Your sandbox (write ONLY here)
- WORK: `agentize/work/bridge/` (a private copy of `bridge/`, no `target/`)
- READ-ONLY: `agentize/harness/`, `agentize/prompts/`, `agentize/AGENTS.md`
- FORBIDDEN: any other path on disk, all git operations, other agents' sandboxes.

## What you are refactoring
Everything under `agentize/work/bridge/src/` plus `build.rs`/`ui/app.slint`
(keep the Slint API compatible with the Rust it generated). Today it is already
split into `main.rs` (thin UI), `app.rs` (state), `core.rs` (methods),
`server.rs` (REST), `net/` (driver + phone + telemetry). Push it further:
readable names, tighter modules, smaller functions, clear error handling,
remove duplication, extract helpers, add *why* comments. You may:
- split/merge modules, move functions between files, extract submodules
- rename internal types/functions/fields (anything not in the locked contract)
- restructure state flow (e.g. how `AppState` is written/read), add tests
- restructure the REST handler internals and the mock-proof seams

## The locked contract (change NOTHING here)
- `Cargo.toml` dependencies/features/packages (do not add crates, do not bump)
- CLI: `--headless` flag, default and `CARDBOARD_BRIDGE_PORT` env
- ports: telemetry 42070 discovery / 42071 telemetry; video (42069) is untouched by design
- **byte-exact wire strings**: `BRIDGE_HELLO v1`, `BRIDGE_ACK`, `CARDBOARD_CAP`,
  `BRIDGE_CFG`, `CARDBOARD_PHONE_HELLO`, telemetry byte format (0x10 gyro / 0x11
  hand / 0x20 ping) parsing and emission
- REST contract: paths `/`, `/health`, `/status`, `/logs?n=`, `POST /settings`,
  the JSON field names (e.g. `app_version`, `driver_connected`, `packets_total`,
  `sent.width`, …), status codes
- every log line the transcripts show (the harness diffs them byte-for-byte):
  `bridge started`, `driver handshake established`, `phone hello from …`,
  `config applied: …`, `phone timed out`, `driver heartbeat lost`,
  `REST control plane on http://127.0.0.1:8567`, `telemetry listener on udp 42071`, …
- the Slint callbacks/properties `ui/app.slint` declares are a stable API to the
  Rust side; keep them compiling with `slint-build`

## Behavior equivalence gate (THE stop condition)
From the **repo root**, run:
```powershell
powershell -File agentize\harness\verify.ps1 -Module bridge -Mode check
```
The harness builds `cargo build --release` in the sandbox and replays a scripted
session (mock driver on 42070 replying `BRIDGE_ACK` and capturing `CARDBOARD_CAP`/
`BRIDGE_CFG`, mock phone pushing hello/gyro/hand/ping on 42071, REST calls), then
compares the recorded transcript byte-for-byte against the baseline. Timing-only
fields (`gyro_fps`, `hand_fps`, `stream_fps`, `latency_ms`) are masked; everything
else — every status field, every log line, the exact config bytes the driver
received — must be identical.

- Exit `0` = **PASS** → you are done.
- Exit `1` = **FAIL** → something observable changed. Diff the transcript
  (`agentize\harness\results\bridge\check\...` vs `agentize\harness\results\bridge\baseline\transcript.txt`),
  fix it, rebuild, rerun.
- Exit `2` = prereq (Rust toolchain) missing → report, never fake a pass.
- Repeat until `PASS`. This gate is the gold standard of the three — treat it as
  binding proof, not ceremony.

## Before you stop
1. Final `verify.ps1` run prints `PASS` (exit 0). Last action = gate, nothing after.
2. Write `agentize/work/bridge/REFACTOR_REPORT.md`:
   - every module/file you created / deleted / renamed / merged, and why
   - the conventions you established
   - confirmation of the locked-contract items above
   - the exact gate command + last exit code.
3. Do NOT modify the real `bridge/`. Your output lives in the sandbox; a human
   merges it later.