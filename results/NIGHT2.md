# NIGHT2 — overnight regression on main 1aa2b80 (2026-10-14)

Scope: a dated record of the overnight regression run on the test cluster on 2026-10-14, commit 1aa2b80 (after the CKPT, N7 and N7b
merges). Digits only, no writes on the timing runs, no speed claim. Times are Eastern. Dated record; the code and the central docs win
where they differ.

**Gates (measured, 1e9 digits, default settings and the SHMEM launch line):** 1, 2, 4 and 6 nodes PASS, digits identical, segv 0,
rc 0 (06:09–06:11 ET).

**6-node standard repeat (measured, 3.8646 × 10¹¹ digits, defaults, no writes):** 31 runs started, 0 hangs, all rc 0 and VERIFY OK.
Total time mean 190.70 s, sd 12.03 s, min 175.38 s, max 217.78 s (06:15–07:55 ET). These are repeat numbers only; no paired comparison.

**SHMEM-path sweep (measured, `COMM_OFI=0`, seeded list of 24 cases):** 24 of 24 digits identical at 1, 2, 3, 4, 5 and 6 nodes
(4 cases per node count, 05:12–05:49 ET). Totals 14.8–81.6 s (mean 32.0 s, sd 16.6 s), which mix sizes and are not a benchmark.

## Observation: nodes drained after our jobs ended (reported from the cluster's node records; cause unknown, site-side)

- The 6 nodes used by a job went to drain with `needs_reboot: reboot issued: reboot timed out` right after three of our jobs ended:
  - 2026-10-13 21:07 ET, after a SHMEM run killed at its timeout;
  - 2026-10-14 02:05 ET, after a run killed at its timeout;
  - 2026-10-14 09:06 ET, after a job that ended normally.
- Other jobs that ended normally were not followed by a drain. The first two drains followed killed runs; the third followed a
  normal end, so a killed run does not account for all of them.
- Cause unknown; the reboot timeout is reported by the site, so this needs a look on the site side. No action taken on our side.

## RESUME

- Source summaries are on the test cluster in our day-plan directory (not in the repo). The record is complete; nothing to resume.
