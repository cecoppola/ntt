---
name: aac7q-no-overwrite-running
description: "never qadd over a queue file that may be running (bash reads it live); to withdraw a started item move the queue file away before scancel, else the runner resubmits it"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-20T07:19:21.966Z
---

On 2026-10-20 I regenerated 9hn (added counter hooks) and qadd-copied it over ~/aac7q/queue/9hn_*.sh seconds after the runner had started it; bash, reading the running script incrementally, hit a syntax error and the item ended FAILED_screen with 0 runs (re-queued as 9hp).

**Why:** the runner executes `bash $AQ/queue/<item>.sh`; an in-place copy changes the bytes under the running interpreter.
**How to apply:** to change a queued item, first check `squeue` and that no `<item>.rc`/`.log` exists; if it might have started (or will start within seconds, e.g. nodes just freed), give the new version a NEW name instead of overwriting. Related: [[aac7q-name-order]].

Also (2026-10-20): `scancel` of a just-started item makes the runner resubmit it (9ju came back as a new job). To withdraw a started item, first move its queue file to `~/aac7q/withdrawn_*`, then scancel the single job ID.
