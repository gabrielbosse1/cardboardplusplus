# Mission (Agent 2 — Android App)

You are one of **three** AI agents refactoring Cardboard++ **in parallel**.
Your module is the **Android app** (`cardboardplusplus-android/`, built with the
`sdk/` module and the gradle root). Your job: make its code much more readable
and maintainable via a **strictly behavior-preserving refactor** — and prove it
with the harness.

## Read first (mandatory)
`agentize/AGENTS.md` — the isolation contract and the meaning of the gates.

## Your sandbox (write ONLY here)
- WORK: `agentize/work/android/` (gradle root + `sdk/` + `cardboardplusplus-android/`)
- READ-ONLY: `agentize/harness/`, `agentize/prompts/`, `agentize/AGENTS.md`
- FORBIDDEN: any other path on disk, all git operations, other agents' sandboxes.

## What you are refactoring
Everything under `agentize/work/android/cardboardplusplus-android/src/` and the
native/JNI glue it owns. Targets: readable names, smaller files/classes, clear
package ownership, consistent Kotlin/Java style, fewer god-classes. You may:
- split files/classes, rename classes/packages/functions/fields (internal)
- reorganize the `src/main` tree into clearer package hierarchies
- extract helpers, move listeners out of activities, add *why* comments
- touch `build.gradle` ONLY to add the same dependencies needed elsewhere, never
  versions or plugins. Prefer: do not touch build files at all.
- keep the native CMake target list accurate if you move `.cpp` files

## The locked contract (change NOTHING here)
- `applicationId`, `namespace`, `versionCode`/`versionName`, min/target SDK
- every component/permission/feature in `src/main/AndroidManifest.xml` that is
  declared — they drive the badging gate. If you MUST move an Activity into a new
  package, update its manifest reference accordingly (the gate then proves the
  final merged manifest is still identical).
- gradle plugin versions, dependencies, versions, `ffmpeg-kit` etc., proguard file
- the runtime wire protocol (UDP text frames, discovery strings, telemetry bytes)
  and the `AndroidManifest`-visible world: no new permissions, no removed ones
- JNI symbols and native `.so` linkage (the app links `libGfxPluginCardboard.so`
  and any ffmpeg-kit native pieces — do not change native ABI surface)

## Behavior equivalence gate (THE stop condition)
From the **repo root**, run:
```powershell
powershell -File agentize\harness\verify.ps1 -Module android -Mode check
```
The harness builds `:app:assembleDebug` in the sandbox, then compares against
the baseline: (1) zero build errors, (2) identical merged APK contract via
`aapt dump badging` (package, supplied capabilities, permissions, native-code,
launchable activity).

- Exit `0` = **PASS** → you are done.
- Exit `1` = **FAIL** → a contract changed (e.g. a manifest rename you missed,
  a permission you touched, a dep you bumped). Find it, fix it, rebuild, rerun.
- Exit `2` = prereq missing. Check for the Android SDK: either set the `ANDROID_HOME`
  environment variable before relaunching, or create `local.properties` at the
  sandbox root (`sdk.dir=C\:\\path\\to\\android-sdk`). NDK must be installed in it.
  Then rerun `agentize\setup.ps1` to (re)record the baseline and continue.
- Repeat until `PASS`. Run the gate after meaningful chunks, not only at the end.

## Before you stop
1. Final `verify.ps1` run prints `PASS` (exit 0). Last action = gate, nothing after.
2. Write `agentize/work/android/REFACTOR_REPORT.md`:
   - every file/class/package you created / deleted / renamed / merged, and why
   - the style + package conventions you established
   - confirmation of the locked-contract items above
   - the exact gate command + last exit code.
3. Do NOT modify the real `cardboardplusplus-android/`, `sdk/`, or gradle root.
   Your output lives in the sandbox; a human merges it later.