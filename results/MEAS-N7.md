# MEAS-N7 — SHMEM data path (`COMM_OFI=0`) against libfabric (`COMM_OFI=1`) at 2 nodes
**Question.** OPPORTUNITIES N7: how much slower is the SHMEM-only data path than `comm_ofi` today, where does the gap sit, and what SHMEM pool/heap does the plan need with `COMM_OFI=0`?
**Change / branch.** None (measurement only). Build: `main` a6ba51d, `make -B SHMEM_CRAY=1` (log has `-DCOMM_SHMEM`), aac7 worktree `~/ntt-wt/meas`. Switch `COMM_OFI` (unset = 1 where a cxi NIC exists).
**Method.** Queue item `82_meas_n7` (user-approved 2026-10-12), one 2-node hold, driver `meas_abba.sh`: rounds of A B B A, A = `COMM_OFI=1`, B = `COMM_OFI=0`; 2 x 6.441e10 = 1.2882e11 digits, no digit writes; launch line = `tools/profile_load.py aac7 --line` (+ `COMM_XSTATS=1 MN_WAIT_STATS=1`); `mnrun.sh` recomputes the SHMEM pool/heap per arm from `MN_PLAN_ONLY`. VERIFY OK and rc 0 on all 8 runs. Round cap: the 40-min item budget allowed 2 rounds (SHMEM runs take 2 to 3 times longer), not the 4-round cap. Plan values: `tools/plan_values.py --profile aac7 128820000000 2` with `COMM_OFI=1` / `0` (`COMM_OFI_PLAN_CXI=1`). Digits were not compared (timing runs); VERIFY OK only.
**Results (measured).**

| run | A `COMM_OFI=1` total s | B `COMM_OFI=0` total s |
|---|---|---|
| round 1 | 157.39, 133.32 | 285.22, 280.26 |
| round 2 | 136.25, 134.11 | 406.20, 278.16 |

| statistic | value |
|---|---|
| paired round means B - A (2 pairs) | +137.4 s, +207.0 s |
| mean difference B - A | **+172.2 s, 95% CI [-270, +614] (n = 2 pairs, p 0.12)** |
| means of the 4 runs | A 140.3 s, B 312.5 s (B median-like value 281 s: 3 of 4 runs within 278 to 285 s; one run 406 s with init 64.7 s and `ready` waits doubled, a slow-mode run) |

The interval is wide only because of the 406 s run and n = 2; every B run is at least +122 s above every A run (smallest B 278.2 s, largest A 157.4 s), so the sign is not in doubt. Without the 406 s run the typical gap is about +145 s (2.1 x).

Per-stage split (seconds in the run's own phase line; mean of A over 4 runs, B over the 3 runs of ordinary speed; wait = the wait-stats sum over the 4 APU threads of the slower node):

| stage | A (OFI) | B (SHMEM) | B - A |
|---|---|---|---|
| init | 34 to 44 | 34 to 40 | about 0 |
| binary splitting (bs) | 45.5 | 77.1 | +31.6 |
| distributed division multiply (dm) | 54.4 | 163.0 | +108.6 |
| dc (digits) | 3.1 | 3.0 | about 0 |
| wait, bs / dm / recip (thread-summed) | 52 / 105 / 34 | 188 / 435 / 148 | x 3.6 / 4.1 / 4.4 |

So the whole gap is exchange wait: dm carries about 77 % of it, bs about 22 %, the rest none; compute and init are unchanged. (Phase sums do not add to total because of the unmeasured recip inside dm; see the run line.)

SHMEM pool and heap the plan needs (modelled by the planner, used by the runs), 1.2882e11 digits, 2 nodes:

| | `COMM_OFI=1` | `COMM_OFI=0` |
|---|---|---|
| `COMM_SHMEM_POOL_MB` | 512 (plus 4 comm_ofi pools of 2304 MiB = 9216 MiB device) | 8704 (8192 MiB staging = 4 x 2048 + control) |
| `SHMEM_SYMMETRIC_SIZE` = `XT_SYMMETRIC_HEAP_SIZE` | 1024M | 9216M |
| plan check | OK | OK |

At the target (OPPORTUNITIES, modelled earlier): pool 2304 to 10240 MiB, heap 10752 MiB.

**Verdict.** SHMEM-only is 2.1 x slower than `comm_ofi` at 2 nodes (+145 s typical, about +172 s with the slow run; wait-bound in dm and bs), so a SHMEM-primary target needs the data path fixed first (N7 follow-ups: both NICs, fewer round trips). Keep `COMM_OFI=1` as the default. The pool/heap with `COMM_OFI=0` is ready: 8704 / 9216M at this size and the plan stays OK.
**Open issues.** Only 2 pairs (time budget); the mean's CI is uninformative, the per-run ordering is the evidence. 2 nodes only; the 10-node ratio was 2.1 x (OFI17), consistent. OpenSHMEMX 12.1.0 and both NICs per PE untested. Digits against the reference not compared.
**RESUME.** Raw: aac7 `~/meas/N7/{totals.tsv,stats.txt,log/}`. Nothing running. Next: N7 fix experiments (aac7 2 to 6 nodes) if the user wants them.
