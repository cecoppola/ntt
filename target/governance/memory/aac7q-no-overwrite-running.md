---
name: aac7q-no-overwrite-running
description: "never qadd over a running queue file; withdraw = move queue file, scancel, and for session holds kill the login-node loop PID (it outlives scancel)"
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

Session holds (2026-10-25): the hold script `bash ~/aac7q/queue/9zu_sessionNN.sh` runs on the LOGIN node; scancel ends the Slurm job but not that loop, which keeps taking items from its bundle and runs them with no hosts (c4b_cap30: 84 runs rc 1). Release a hold = move the queue file to withdrawn/, scancel the job, then `ps -u <me> -o pid,args | grep sessionNN` and kill that single PID.
Find the loop PID with a pattern that cannot match your own command line (2026-10-25: `grep "[q]ueue/9zu_session38.sh"` matched the remote shell's own `mv queue/9zu_session38.sh ...` and killed it mid-command): run the ps/kill in its own c7 call, or use `pgrep -f '^bash .*/aac7q/queue/9zu_sessionNN\.sh$'`.
