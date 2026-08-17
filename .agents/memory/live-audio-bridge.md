---
name: Live audio bridge commands
description: Durable protocol boundary for the browser/ESP32 WebSocket bridge and Gemini Live.
---

Bridge control messages such as stream start, stream end, and keepalive are local
commands. They must be handled by the bridge and never forwarded as arbitrary
JSON to Gemini Live, whose protocol rejects unknown fields and closes the
upstream session.

**Why:** Forwarding a browser stream-start command caused Gemini to close with
an invalid-payload error, which surfaced to users as intermittent no-response.

**How to apply:** When adding client-side WebSocket controls, classify them as
bridge commands first; only forward documented Gemini Live protocol messages
upstream.