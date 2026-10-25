---
name: no-admin-requests
description: "Do not propose or draft requests to the aac7 cluster admins; work with the existing node settings (THP, limits, no reserved huge pages)"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-18T05:35:41.498Z
---

Do not propose asking the aac7 admins for changes (reserved huge pages, prolog compaction, more nodes, node fixes).
Work within the settings we have and find user-side solutions.

**Why:** user, 2026-10-18 01:40 ET: "We have to work with the settings we have. You don't need to continue making
admin requests." Earlier answers in the same direction: D33 (tabled), D40 (work with whatever the limits are), D43.

**How to apply:** when a fix would need root, say so in one line only if it explains a limit, then pursue the best
user-side option. Fragmenter experiments need no node cleanup for later users (D42). Related: [[aac7-packing]].
