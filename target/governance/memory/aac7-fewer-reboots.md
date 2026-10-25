---
name: aac7-fewer-reboots
description: aac7 NIC losses follow epilog reboots after our job ends; run many items inside one long hold (session item + ~/aac7q/bundle/) to cut reboots
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-21T04:59:20.543Z
---

Minimise job ends on aac7: every job end of ours usually triggers a site epilog reboot (numa_miss > 1000 pages), and NICs are lost at ~1 in 28 reboots (permanent so far; 9 of 14 nodes dead by 2026-10-21). User (2026-10-21): "You must continue work. But, is there a way for you to reduce the risk of crashing the network cards going forward?"

**Why:** fewer reboots = fewer NIC losses; a lost NIC takes a node out for good, and node count limits all experiments.

**How to apply:** run a long session hold (template: day/multi/items/9n0_session3.sh) that executes item scripts dropped into aac7 `~/aac7q/bundle/` in name order inside one hold, with per-node NIC counts before each item (`~/day/session3/NICS`); add work with cp7 into bundle/, not qadd. Avoid arms known to abort or segfault. Related: [[aac7-packing]], [[rearm-watcher]].
