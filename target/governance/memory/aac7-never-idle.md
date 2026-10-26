---
name: aac7-never-idle
description: "Standing order — whenever aac7 nodes are unused, queue a useful task without asking first"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-21T05:45:59.780Z
---

User, 2026-10-21 01:55 EDT (verbatim): "If aac7 is unused, always queue a useful task on it."

**Why:** I left aac7 idle while waiting for a design approval and asked whether to queue a diagnosis; the user does not want to be asked — idle nodes are waste.

**How to apply:** at every event, if healthy aac7 nodes are free, queue the most useful runnable item (reference measurements, risk-reducing probes, A/A controls) at once and report it afterwards; do not ask permission for that. Still run work inside one long session hold (bundle) to keep job ends few. Related: [[aac7-fewer-reboots]], [[rearm-watcher]], [[aac7-packing]].
