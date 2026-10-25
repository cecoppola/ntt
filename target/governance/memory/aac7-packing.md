---
name: aac7-packing
description: "aac7 queue must run 2-6 concurrent items (shapes 4+2 ... 1x6) while the per-user limit is 6 nodes; never idle, never one big job by default"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 774b5c91-540b-4adc-8faf-658f932531db
  modified: 2026-10-18T21:54:40.989Z
---

Until the aac7 per-user node limit changes from 6, run as many queue items at once as fit (2-6 concurrent), using the
shapes 4+2; 2+2+2; 4+1+1; 2+2+1+1; 2+1+1+1+1; 1+1+1+1+1+1. A single 6-node item only for final confirmations against
6-node baselines. Keep the queue stocked so every free node has a runnable item, and arm an in-session wait so finished
items are collected and replaced at once. Rule text lives in $APU/CLAUDE.md (Work, Clusters).

**Why:** the user (2026-10-17) found work slowed badly because agents ran one 6-node job at a time and left the queue
idle; 6 nodes = 24 APUs, enough for several lines of work at once.

**How to apply:** size digits per node count, rotate arms across slots, include an A/A control arm, use GROUP headers
so concurrent arms start together (runner supports backfill and GROUP since commit 36bd697).

**Diagnosis size (user 2026-10-17 22:10 ET):** 4-node tests are much more useful than 6-node ones: they leave 2 nodes for
other work (4+2). The SHMEM gap and stalls first appear at 4 n (none at 1-2 n), so diagnose there; keep isolating the
source until it can be fixed, not just measured.

**Never wait on a build (user 2026-10-18 ~21:50 ET, "explain why you aren't running anything in the queue"):** when the
next planned item still needs code, queue an item that needs none (env sweep, A/A repeat, size check) in the meantime.
Keep items short (user: 35-45 min is too long; aim <= 15-20 min) and size them from the slowest measured runs. Related:
[[ecalc-restart-note]].
