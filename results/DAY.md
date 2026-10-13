# DAY - the 2026-10-13 day plan: large run, correctness sweep, SHMEM sweep, 6-node repeat

**Scope.** Four queued items on the test cluster, run 2026-10-13 between 08:54 and 16:40 ET, on commit 209712f
(`origin/main`, SHMEM build with `-DCOMM_SHMEM`; the large run used commit b1e4d567). Question: do digits stay identical at a large size, over a random sweep of 1 to 6 nodes, with the
SHMEM path (`COMM_OFI=0`), and what are the 6-node times with the current defaults. Collection commands and the item plan
live in the private day plan. Times are Eastern. All numbers are measured unless labelled otherwise.

## 1. Large run (6 nodes, 3.8646 × 10¹¹ digits, writes)

| item | result |
|---|---|
| write run | rc 0, `VERIFY=OK`, total 1793.37 s (wall 1805 s, 09:37 ET); bs 90.1 s, recip 21.7 s, dm 697.2 s, init 41.5 s |
| digits vs reference | first 1 × 10¹¹ digits identical (cmp, 738 s); output 322 GB, deleted after the compare |
| `ECALC_RECHECK=1` on the written files | **not performed**: the recheck read a path that does not exist (`large_recheck.out.t1`), "no usable sidecar" on all six nodes, rc 1 after 8 s |

- The recheck failure is in the item's harness (the file prefix did not match the written files), not in the digits: the
  compare against the reference passed. The recheck itself is still open (follow-up 1).

## 2. Correctness sweep (1 to 6 nodes, 48 runs, digits 1 × 10⁹ to 1 × 10¹¹)

| runs | ok | bad | node counts (runs each) | total time, ok runs (measured) |
|---|---|---|---|---|
| 48 | 48 | 0 | 1, 2, 3, 4, 5, 6 (8 each) | mean 63.3 s, sd 57.4, min 12.4, max 240.4 |

- Every run: rc 0, `VERIFY=OK`, digits identical to the reference, no segfault. Sizes are random and not comparable as times.

## 3. SHMEM path sweep (`COMM_OFI=0`, 24 runs, 1 × 10⁹ to 2 × 10¹⁰ digits)

| runs | ok | bad | node counts | total time, ok runs (measured) |
|---|---|---|---|---|
| 24 | 21 | 3 | 1, 2, 3, 4, 5, 6 (4 each) | mean 33.2 s, sd 19.8, min 11.2, max 77.5 |

- The 3 failures are the first three runs (12:55 ET, at 3, 4 and 6 nodes): rc 141 within 8 to 18 s, `segv=3`. The SHMEM
  library saw one network interface on one node while four were requested. This is a node fault, not a code fault; the other
  21 runs at 1 to 6 nodes were digit-identical. Follow-up 2 (node-screen check of network interfaces).

## 4. Final: gates and 6-node repeat (defaults, 3.8646 × 10¹¹ digits, no writes)

| check | result |
|---|---|
| gates at 1, 2, 4, 6 nodes on 209712f | PASS, digits identical |
| 6-node repeats | 34 runs, 34 OK, 0 hangs, rc 0 throughout |
| time (measured) | mean 197.1 s, sd 12.0, min 181.1, max 224.8 |
| previous night (results/NIGHT.md §5) | mean 202.1 s, sd 8.6, 47 runs |

- The two means are not a paired comparison (different commits and dates, different run counts). No speed claim is made from
  them; a claim needs paired ABBA rounds.
- The test cluster sat idle from 13:20 to 14:39 ET while the final item was held for its start time (coordinator hold logic).

## 5. Decision

- D27 (a), keep the summed SHMEM pool (target 17664 MiB, about +7.25 GiB per node, modelled), stands. No change to the
  defaults in this record.

## Follow-ups

1. Re-run the `ECALC_RECHECK=1` check on the 6-node written files with the correct prefix (open; the write run and the
   compare passed).
2. Node-screen network-interface check before SHMEM runs (task NIC; the cause of the 3 failures is a node fault, not measured
   as fixed).
3. Idle time of about 80 minutes on the test cluster between the sweep and the final item: scheduling, not code.

## RESUME

Branch `daydoc` from `origin/main` 209712f; this file, the RESULTS.md index row and the PLAN.md §2 repeat line are the only
changes. Nothing to resume unless follow-up 1 is run.
