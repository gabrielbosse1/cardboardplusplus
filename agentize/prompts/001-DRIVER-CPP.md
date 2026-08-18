# Mission (Agent 1 — C++ SteamVR Driver)

You are one of **three** AI agents refactoring Cardboard++ **in parallel**.
Your module is the **SteamVR C++ driver**. Your job: make its code much more
readable and maintainable via a **strictly behavior-preserving refactor** — and
prove it with the harness.

## Read first (mandatory)
`agentize/AGENTS.md` — the isolation contract and the meaning of the gates.

## Your sandbox (write ONLY here)
- WORK: `agentize/work/driver/` (a private copy of `driver_cardboardplusplus/`)
- READ-ONLY: `agentize/harness/`, `agentize/prompts/`, `agentize/AGENTS.md`
- FORBIDDEN: any other path on disk, all git operations, other agents' sandboxes.

## What you are refactoring
Files currently in `agentize/work/driver/src/` and `agentize/work/driver/include/`:
`DeviceFactory.cpp`, `DeviceProvider.cpp/.h`, `HmdDriver.cpp/.h`,
`ControllerDriver.cpp/.h`, `VideoEncoder.cpp/.h`, plus `driver_cardboardplusplus.sln/.vcxproj`.
Targets: readable names, one responsibility per file, smaller functions, clear
ownership, sane include layout, consistent style. You may:
- split translation units (e.g. `HmdDriver.cpp` into hmd/, encoding/, compositing/)
- add or reorganize headers, extract helpers, add scoped namespaces
- rename **internal** symbols, types, members, constants, files
- add comments explaining *why*
- add new files to the `.vcxproj` as needed

## The locked contract (change NOTHING here)
- `HmdDriverFactory` export name + signature (entry point SteamVR loads)
- the built DLL name (`driver_cardboardplusplus.dll`), target name in the vcxproj
- all OpenVR `vr::` interface signatures you implement/use
- every wire-visible string: `CARDBOARD_CAP`, `BRIDGE_CFG`, `BRIDGE_HELLO`,
  `BRIDGE_ACK`, `CARDBOARD_DISCOVERY`, and the ports 42069 / 42070 / 42071
- `resources/driver.vrdrivermanifest` and `resources/controller_profile.json`
  (byte-identical — the harness hashes them)
- build settings: preprocessor defines, link flags, ffmpeg/openvr libraries,
  configuration/platform layout in the vcxproj (you may only *add* source files)
- any public API surface in `include/` that other code or tooling relies on
  (headers can be reorganized into new files, but exported symbols and the
  SteamVR-facing contracts must not change)

## Behavior equivalence gate (THE stop condition)
From the **repo root**, run:
```powershell
powershell -File agentize\harness\verify.ps1 -Module driver -Mode check
```
The harness builds `Release|x64`, then compares against the baseline:
1. zero build errors,
2. identical DLL export table (`dumpbin /exports`),
3. identical hashes of the resource files.

- Exit `0` = **PASS** → you are done.
- Exit `1` = **FAIL** → a contract changed. Find it, fix it, rebuild, rerun.
- Exit `2` = prereq (Visual Studio toolset) missing → do not fake a pass; report.
- Repeat until `PASS`. Keep running the gate after every meaningful chunk of work,
  not just at the end.

A clean gate is necessary proof of behavior preservation — because the driver is
loaded by SteamVR there is no scripted runtime harness for it, so the export +
resource gates plus your own discipline are the guarantee. If you ever believe a
change is behavior-affecting even though the gate passes, undo it or call it out
loudly in your report instead of letting it through silently.

## Before you stop
1. Final `verify.ps1` run prints `PASS` (exit 0). Last action = gate, nothing after.
2. Write `agentize/work/driver/REFACTOR_REPORT.md`:
   - every file you created / deleted / renamed / merged, and why
   - the naming/structure conventions you established
   - confirmation of the locked-contract items above
   - the exact gate command + last exit code.
3. Do NOT modify the real `driver_cardboardplusplus/`. Your output lives in the
   sandbox; a human merges it later.