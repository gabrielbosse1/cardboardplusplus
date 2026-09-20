# Fix-risk review — 2026-09-19
Some audit fix suggestions are correct in isolation but break specific setups.
Original suggestion → break scenario → corrected suggestion.

## R1. Stop discovery after N ACKs (android 5.1, cross F25 bridge-as-broker)
- Original: stop broadcast after e.g. 3 ACKs; bridge tells driver phone IP.
- Breaks: driver restart / SteamVR reload clears `m_hasPhoneTarget` (Discovery.cpp:109,228-239 needs a new discovery packet to relearn IP). Phone stopped → never recovers, black screen until app restart. Also breaks DHCP roam and second-PC switch. DiscoveryManager.java:19-21 explicitly relies on re-announce every 60 ACKs so a restarted driver learns the cap.
- Corrected: keep broadcast but back off: full rate (500ms) until first ACK, then low-rate heartbeat (1 per 5s) + CAP re-announce every 60 ACKs. Watchdog stall or 5s without ACK → resume full rate immediately. `pokeNow()` (cross F12) for immediate single probe, not as replacement for the loop. Bridge-broker stays fallback, never sole path.

## R2. Strict CARDBOARD_DISCOVERY token only (driver M10)
- Original: require prefix, drop everything else.
- Breaks: old APKs that rely on fall-through (current phone sends DISCOVERY today so OK now, but version skew with F16 unversioned wire breaks them silently). Also `test_wire_protocol.cpp:560-566` asserts any-packet ACK — strict change breaks test + hides compat.
- Corrected: allowlist `CARDBOARD_DISCOVERY` + `CARDBOARD_PHONE_HELLO` variants for target switch; keep CAP/KEYFRAME/BRIDGE_* handling as-is; log-and-rate-limit unknown instead of ACK-switch. Update test to assert strict + legacy-tolerant behavior.

## R3. Gate encoder on m_streamEnabled (driver H2)
- Original: check flag in Present()/EncodePendingFrame().
- Breaks: `m_streamEnabled` defaults ON (HmdDriver.h:217 =1) but Activate never resets (L14). One OFF from Bridge sticks across SteamVR reloads → video dead with green UI. SHM skip bug (M15) can also drop the later ON.
- Corrected: reset to 1 in Activate, gate only in Present() (single load), log ON/OFF transitions, treat no-settings-yet as ON. Fix M15 alongside.

## R4. Refresh driver liveness on any STATS (bridge F27)
- Original: refresh `last_ack` on any valid datagram, return Continue.
- Breaks: `poll_for_ack` ignores `_src` (driver.rs:96). Any local process sending `BRIDGE_STATS ...` keeps `driver_connected` true after real driver died.
- Corrected: Continue for STATS yes, but refresh liveness only if `src == 127.0.0.1:DRIVER_DISCOVERY_PORT`. Same for ACK. Keep `bridge_stats_does_not_touch_connection_flags` semantics for spoofed source.

## R5. Update phone_ip on every packet (bridge F30)
- Original: update on every non-Unknown.
- Breaks: Ping is 1 byte 0x20 (telemetry.rs:85), Hand/NetStats trivially forgeable. Any LAN device spamming Ping steals `phone_ip` shown in UI and any future unicast path.
- Corrected: update IP only on Hello/Gyro/Rotation (current). NetStats/Hand update only if `phone_connected` and src IP already equals current (roam needs Hello/gyro to move). Never on Ping/Unknown.

## R6. Liveness stamp scope (bridge F29)
- Original audit direction is right (garbage in phone.rs:62 stamps before parse), but do not exclude Ping.
- Corrected: stamp `last_seen` on Hello/Ping/Gyro/Hand/Rotation/NetStats; ignore Unknown only. Ping is the intentional keepalive (telemetry.rs:14).

## R7. Stop forwarding 0x10 to driver (cross F20)
- Original: drop 0x10, gate validity on rotation only.
- Breaks: GetPose has two gates — `fresh` from `m_lastSensorRecvMs` (any) + `rotFresh` from `m_lastRotationRecvMs` (HmdDriver.cpp:261-268). Rotation-vector rate varies (50-200Hz, batching). Gyro-only devices (no game vector) lose tracking entirely instead of degrading. Also removes bridge fps-pill input if pill counts forwarded types.
- Corrected: forward 0x12 always; forward 0x10 throttled to ~10Hz for diagnostics. Split validity: pose valid if rotation fresh; if device never sent 0x12, fall back to gyro-fresh with `OutOfRange` + log which path gates. Makes quat loss visible without bricking gyro-only phones.

## R8. Min decoder cap across all decoders (android 7.3)
- Original: take min across HW AVC decoders.
- Breaks: thumbnail/low-res secondary decoder drags ceiling to 720p even though the selected player does 4K → driver permanently clamped low.
- Corrected: take min across *realtime-capable* HW AVC decoders for display output, or better the cap of the decoder MediaCodec will select (by name). Never mix W from one decoder + H from another (cross minor note) — use per-decoder max-area.

## R9. 1400B chunks alone (driver H6)
- Original: chunk <=1400B.
- Breaks alone: ~40x more sendto/frame raises pps and worsens VideoReceiver `insert/erase` O(n) memmove (VideoReceiver.cpp:167,197-198) + JNI copies (app.cc:773-778).
- Corrected: ship with Android 6.3 ring/offset buffer + Driver M9 scratch-buffer reuse together. Start 1400B (1200B for Tailscale), measure loss vs CPU.

## R10. Raw 42074 relay (bridge F20/F23)
- Original: `send_to(raw_datagram)`.
- Breaks: forwards malformed/spoofed bytes to driver parser if tag/len unchecked.
- Corrected: validate `len==45&&tag==0x10 || len==25&&tag==0x12` then forward slice untouched. Keeps zero-alloc win without passing garbage.

## R11. dequeueInputBuffer(0)+drop (android 6.4)
- Original: drop under pressure.
- Breaks: dropping SPS/IDR means decoder never configures → stall misread as network loss.
- Corrected: drop P-frames only; never drop keyframes; keep short blocking wait for keyframes, 0-timeout for P.

## R12. Fail stream when sidecar missing (cross F24)
- Original: fail visibly, imply block.
- Breaks: video-only users bricked by missing Python/model even though stream is fine.
- Corrected: expose `hand_pipeline_ok` in UI/REST, warn loudly, keep stream running. Block only if user explicitly enabled Camera/hand overlay.

## R13. Set both pose fields blindly (driver H1)
- Original: set qDriverFromHeadRotation = quat.
- Risk: convention differs (driver-vs-head vs tracker legacy); blind set could double-apply in some apps.
- Corrected: set both to same quat (matches sample drivers) but verify with phone-rotate + SteamVR mirror check before/after; keep legacy qRotation in sync.
