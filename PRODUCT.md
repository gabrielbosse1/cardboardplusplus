# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Users

- **Primary:** Anyone with a Google Cardboard headset and a gaming PC running SteamVR. Ranges from complete "normies" who need guided setup to experienced developers and VR tinkerers who want a skip button.
- **Secondary:** Developers building VR projects on a cheap, open, hackable platform.

## Product Purpose

Cardboard++ brings high-end VR features — hand tracking, finger tracking, SteamVR compatibility — to the cheapest VR hardware: Google Cardboard. The desktop app (Cardboard++) is the installer, hub, and control plane for the entire system. It walks first-time users through setup, installs the driver and app, and lives as the ongoing monitoring and settings surface.

## Positioning

Cardboard++ is the only open-source, full-package solution that turns Google Cardboard into a SteamVR-compatible VR system with hand and finger tracking. Alternatives like ALVR or Vridge mirror screens; Cardboard++ is a complete product with a desktop hub, SteamVR driver, and Android app working together — one install, everything works (goal; today it is development-only, see Constraints below and README).

## Operating Context

- User has a gaming PC (Windows, SteamVR installed), a Google Cardboard headset, and an Android phone.
- First-time flow: download via QR code → installer → guided walkthrough (install driver, launch SteamVR, connect phone).
- Ongoing use: Cardboard++ desktop app stays open as the hub — monitoring connection status, adjusting stream/tracking settings, viewing diagnostics.
- Developer flow: skip the walkthrough, go straight to the hub.

## Capabilities and Constraints

- SteamVR driver (C++ DLL) captures frames, encodes H.264, streams to phone over UDP.
- Android app decodes H.264, renders VR with Cardboard lens distortion, streams camera JPEG back.
- Desktop hub (Rust + Slint) owns all configuration, installs driver/APK, monitors everything.
- Hand tracking via MediaPipe on the Bridge (TCP sidecar), exposed as SteamVR tracked objects.
- Finger tracking planned (not yet implemented).
- 6DoF tracking planned (IMU-based, needs porting).
- Video stays on UDP. Shared memory for control/telemetry only.
- Windows-only for now; Linux planned after core fixes.
- The system is not yet ready for end-user installation — currently development-only.

## Brand Commitments

- Name: Cardboard++ (final, not changing)
- Logo: `logo.png` in repo root

## Evidence on Hand

- `logo.png` — product logo
- `docs/PROJECT_VISION.md` — detailed architecture and vision document
- Working prototype: Bridge UI, SteamVR driver, Android app all functional
- README describes current state, build process, and project structure

## Product Principles

1. **The hub is the product.** Cardboard++ (the desktop app) owns everything — installs, settings, monitoring, the on/off switch. No feature is done until it works from the hub.
2. **Stream and tracking ship together.** Hand/finger tracking and video streaming always activate together. A stream with no tracking is not a working feature.
3. **Guide the normie, skip for the expert.** First-time users get a walkthrough. Experienced users get a skip button. No forced paths.
4. **Open and hackable.** GPL-licensed, community-driven. Every component is inspectable and modifiable.
5. **Cheap VR should feel premium.** The UX should be polished, not "good enough for a hack." Dark theme default, light theme option, clean layout.
