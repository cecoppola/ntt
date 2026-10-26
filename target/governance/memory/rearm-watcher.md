---
name: rearm-watcher
description: "after every aac7 watcher event, re-arm rcwait immediately and keep >=2 h of items queued; a missed re-arm idled aac7 5 h"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-19T12:37:21.329Z
---

After reading any `rcwait` notification, re-arm `/home/machinus/apucode/bin/rcwait` (run_in_background) in the same turn, before anything else, and keep at least ~2 h of runnable items queued on aac7.

**Why:** 2026-10-19 I read the 02:42 ET event without re-arming; no later event woke the session, the queue drained at 03:17 and aac7 sat idle until 08:36 ET (5 h 20 min) during an overnight plan.

**How to apply:** every watcher-completion turn starts with re-arming; before ending a turn, check the queue holds work beyond the running items. Related: [[aac7-packing]].
