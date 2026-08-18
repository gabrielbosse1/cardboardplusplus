//! Human-readable index page served at / and /index -- the only
//! documentation of the control-plane API. The harness fingerprints its exact
//! bytes, so keep this text byte-for-byte stable.

pub const ENDPOINT_INDEX: &str = "\
Cardboard++ Bridge — REST control plane
========================================

The bridge runs its control logic (driver heartbeat, phone telemetry, config
push) in a UI-independent core. This server exposes that core over plain
HTTP + JSON on 127.0.0.1. It never touches the encoded video stream (UDP 42069).

Endpoints
---------
  GET  /health   {\"ok\":true,\"app_version\":\"...\"}
  GET  /status   live status snapshot (driver/phone/encoder, fps, hands, ...)
  GET  /logs?n=50  newest-first log lines
  POST /settings push stream settings to the driver:

         curl -X POST 127.0.0.1:8567/settings \
              -H \"Content-Type: application/json\" \
              -d '{\"width\":2880,\"height\":1620,\"fps\":60,\"bitrate\":20,\"encoder\":\"auto\"}'

       encoder: \"auto\" | \"amf\" | \"nvenc\" | \"qsv\" | \"libx264\" (h264_ prefix ok)
       fields are optional; missing ones keep the current defaults.

Port: set CARDBOARD_BRIDGE_PORT to override (default 8567).
";
