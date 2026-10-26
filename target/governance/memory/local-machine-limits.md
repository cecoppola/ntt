---
name: local-machine-limits
description: "Local laptop is small (7 GB RAM, 4 cores, 11 GB swap); cap concurrent local agents and keep heavy work on aac7"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-18T18:27:09.361Z
---

The local machine ("littleblue") has 7 GB RAM, 4 CPUs, 11 GB swap, and runs VS Code (~1 GB) plus every Claude agent
(~0.3 GB each). On 2026-10-18 it froze between 14:15 and 14:25 EDT and was rebooted; the user said "You crashed the
local machine. Don't do that again." Logs showed no OOM (sar 14:10: 4.5 GB available, no swapping, load < 1), so the
cause is unproven, but the rule stands.

**Why:** user order 2026-10-18 after the freeze.
**How to apply:**
- At most 3 background agents running locally at once (user 2026-10-19, D57; was 2), and only when `free` shows >= 2.5 GB available; queue further work instead.
- No local builds beyond syntax checks (`-j1`/`-j2`, never `make -j8`); real builds go to aac7 (login-node build or the item's build).
- No local memory/CPU tests (fragmenter, populate, ecalc runs, stress tests); test only with tiny sizes or on aac7.
- Say so in every agent prompt. Related: [[aac7-packing]], [[vscode-exthost-oom]].
