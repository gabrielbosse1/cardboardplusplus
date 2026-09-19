# Security Policy

## Supported Versions

Only the latest `master` is supported with security updates.

## Reporting a Vulnerability

Do not open a public issue for vulnerabilities. Use GitHub's private
vulnerability reporting (Security tab → Report a vulnerability) so details
stay private until a fix is available.

## Design Notes

Cardboard++ is a LAN-local system with no authentication on its internal
channels:

- The bridge REST API (`127.0.0.1:8567`) binds localhost only — do not expose
  it to the network.
- UDP ports 42069–42074 carry unauthenticated video, telemetry, and control
  traffic. Only run on a trusted local network.
- The MediaPipe sidecar (TCP 42073) accepts connections from localhost only.
