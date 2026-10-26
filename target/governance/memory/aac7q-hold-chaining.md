---
name: aac7q-hold-chaining
description: a queued follow-on session hold gives up after 5 x 10-min pending timeouts; qadd the next hold only near the current hold's end and verify it starts
metadata:
  type: project
---
The aac7q runner cancels a hold that stays PENDING > 600 s and retries every ~15 min; after 5 pending-timeouts it gives up
(rc 126) silently. A hold also ends early when the next bundle item's 1.5*EST+10 min no longer fits before its T_END.
2026-10-22: 9x2_session11 was qadded at 02:40 behind running hold 9u0, gave up at 03:41; 9u0 ended at 05:26 (END: next item
did not fit) and aac7 idled until a new hold 9x6 was qadded (running 05:33).

**Why:** aac7 must never idle ([[aac7-never-idle]]); the give-up is invisible unless the runner log is read.
**How to apply:** qadd the next hold within ~60 min of the current hold's T_END, or right after it ends; then confirm with a
blocking background check (`squeue ... | grep RUNNING`). Grep `~/aac7q/log` for "gave up" on each watcher event.
