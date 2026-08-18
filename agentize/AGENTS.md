# Agentize — parallel refactor orchestration

This directory lets three AI agents refactor the three modules of Cardboard++
**in parallel, without touching each other**, and only finish when a machine
check proves the code still behaves identically.

## Layout

```
agentize/
  AGENTS.md           <- this contract; every agent MUST read it first
  prompts/            <- the per-module mission prompts (expectation management docs)
    001-DRIVER-CPP.md
    002-ANDROID.md
    003-BRIDGE.md
  harness/
    verify.ps1        <- the Behavior Equivalence Harness (THE stop gate)
    bridge_mock.ps1   <- mock driver + mock phone used to record the bridge transcript
    results/<module>/baseline/   <- the "before" fingerprints (recorded by setup.ps1)
    results/<module>/check/      <- "after" fingerprints while an agent runs (git-ignored)
  setup.ps1          <- one-time: make the 3 sandboxes + record baselines
  work/
    driver/          <- private copy of driver_cardboardplusplus  (AGENT 1 only)
    android/         <- private copy of the gradle root + sdk + app (AGENT 2 only)
    bridge/          <- private copy of bridge                    (AGENT 3 only)
```

## Roles and isolation (MANDATORY)

| Agent     | Sandbox (write only here)              | Module original |
|-----------|----------------------------------------|-----------------|
| Driver    | `agentize/work/driver/`                | `driver_cardboardplusplus/` |
| Android   | `agentize/work/android/`               | `cardboardplusplus-android/` + `sdk/` + root gradle files |
| Bridge    | `agentize/work/bridge/`                | `bridge/` |

Hard rules for every agent:

1. **Never touch** another sandbox, the real module directories, `agentize/harness/`,
   `agentize/prompts/`, `agentize/AGENTS.md`, or the git index. Treat everything
   outside your sandbox as read-only.
2. **Never run another module's harness.** Only your module's `verify.ps1 -Mode check`.
3. **No git operations.** `agentize/work/` is git-ignored and disposable. When done,
   a human merges your sandbox back — you just report.
4. Work only in your sandbox path. Create/delete/rename files freely *inside* it.
5. You are **not done until `verify.ps1` prints `PASS`** for your module.

## The Behavior Equivalence Harness (the tool)

`agentize/harness/verify.ps1` is a golden-master harness: it exercises each module
through its real build + its external contracts and fingerprints the observable
behavior *before* (baseline, recorded at setup) and *after* (your refactor). If the
two fingerprints differ, your refactor changed behavior somewhere the wire or the
compiler can see — keep fixing until the diff is empty.

| Module  | Gates (what "same behavior" means) |
|---------|-------------------------------------|
| driver  | Release|x64 build clean (0 errors); **identical DLL export table** (dumpbin); identical `resources/` files (sha256) |
| android | `:app:assembleDebug` clean; **identical merged APK contract** (`aapt dump badging`) |
| bridge  | `cargo build --release` clean; **byte-identical runtime transcript**: REST `/status`, `/logs`, `POST /settings`, the config bytes the driver received over UDP, and the phone telemetry effect on state (timing fields masked) |

Every gate exists to let you reorganize freely *inside* (move functions to other
files, split translation units, rename symbols) while locking the *outside*:
DLL exports, Android manifest contract, REST/JSON wire format, UDP packet bytes,
ports, CLI flags, log lines, and resource files. Those are the observable behavior.

## Commands

```powershell
# one time, from the repo root (creates sandboxes, records baselines)
powershell -File agentize\setup.ps1

# after your refactor, from the repo root — THE stop gate
powershell -File agentize\harness\verify.ps1 -Module <driver|android|bridge> -Mode check
# exit code: 0 = PASS (done), 1 = FAIL (keep fixing), 2 = prereq missing
```

## Prerequisites by module

- driver: Visual Studio C++ toolset (found automatically via vswhere)
- android: Android SDK/NDK reachable via `ANDROID_HOME` or `local.properties`
  (the repo includes a `local.properties` at the Android sandbox root if present)
- bridge: Rust toolchain (`cargo`)

If a baseline failed at setup because a prereq is missing on this machine, the
corresponding agent's harness will report exit code 2 with an explanation. Set up
the prereq, re-run `setup.ps1`, and continue — but never claim PASS without a
real `verify.ps1` run returning 0.