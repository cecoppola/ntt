---
name: keep-dirs-tidy
description: "user 2026-10-25 wants few, organized folders locally and on aac7; remove worktrees, item scripts and run dirs as soon as they are finished (archive first)"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-25T23:01:26.511Z
---

User 2026-10-25: "I mainly want to reduce the number of unnecessary folders so the directories are more organized and neat."

**Why:** seven ntt worktrees, 137 item scripts and 128 aac7 run dirs had piled up; the user found it disorganized (space was not the concern).

**How to apply:** one main copy each (local `ntt/`, aac7 `~/ntt-acc`); delete a worktree and its branch right after its merge; archive finished queue items, generated plans and aac7 `~/day/<run>` dirs (text results into `ntt-archive/`, md5-checked, MANIFEST entry) as soon as their results are written up, not in batches days later; never touch dirs in use (running holds, gate worktrees, rehearsal trees). Related: [[aac7-never-idle]].
