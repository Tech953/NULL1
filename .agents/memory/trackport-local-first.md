---
name: Trackport stays local-first
description: Product boundary for Trackport's audio handling and native server.
---

Trackport is a local-first product. Preserve its native loopback server and Windows build; do not move audio processing to cloud services or move audio off the user's device.

**Why:** The user identified Trackport as a real product whose users depend on its local processing and native app.

**How to apply:** Keep existing audio processing and server behavior on-device when adding artifacts or porting functionality. Only expose web UI changes that preserve the native local workflow.
