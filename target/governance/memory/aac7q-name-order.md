---
name: aac7q-name-order
description: "aac7 queue runner starts the LARGEST waiting item first (ties by file name), then backfills; 6-node items jump ahead of 4/2-node ones"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-20T06:35:43.479Z
---

The aac7 queue runner (~/aac7q/aac7q.sh, header comment line 10 and the "largest-first" block) sorts waiting items by node count, largest first (a GROUP counts as its members' summed nodes), ties by file name; items that do not fit are skipped and later ones that fit start (backfill) until a skipped item has waited too long. Verified 2026-10-20: 6-node items 9er/9hg started ahead of 4-node items with lower names. (Earlier belief "file-name order" was wrong.)

**Why:** packing depends on which item starts next; size decides first, then name.
**How to apply:** a queued 6-node item takes all nodes as soon as they free up, ahead of 4+2 pairs; to keep 4+2 packing, queue the 6 n item only when it should run next. Keep a 1-2 node item ready for leftover nodes. Pending items do not show in squeue, so report queue depth from ~/aac7q/queue, not squeue. Related: [[aac7-packing]], [[rearm-watcher]].
