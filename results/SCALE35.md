# SCALE35 - scaling points at 3 and 5 nodes
**Question.** What are the totals at 3 and 5 nodes at 6.441e10 digits per node, to complete the 1/2/4/6-node scaling curve of PLAN.md section 2?
**Change / branch.** None (docs only). Commit 39346d03 of `main`, SHMEM build, defaults (`comm_ofi`).
**Method.** aac7, 3 runs per count, no output written, `VERIFY` of every node read from the log; aac7 profile launch line.
**Results.** All 6 runs: VERIFY OK, no segfault (measured). Digits not compared (no write).

| nodes | digits | totals (s) | mean +- sd (s) | bs / recip / dm / init (mean s) |
|---|---|---|---|---|
| 3 | 1.9323e11 | 147.98, 143.11, 152.06 | 147.72 +- 4.48 | 52.7 / 15.7 / 57.4 / 34.0 |
| 5 | 3.2205e11 | 175.90, 177.18, 193.25 | 182.11 +- 9.67 | 68.0 / 21.0 / 77.9 / 32.1 |

With the 1/2/4/6-node means of PLAN.md section 2 (67.6 / 119.4 / 172.2 / 204.6 s): 1 to 6 nodes read 67.6, 119.4, 147.7, 172.2, 182.1, 204.6 s;
efficiency T1/Tn 1.00, 0.57, 0.46, 0.39, 0.37, 0.33 (measured / derived). The curve is monotone; 3 and 5 nodes show no slow mode.
**Verdict.** Data point only; nothing to adopt.
**Open issues.** Three runs per count; the 5-node spread (sd 9.7 s) is the largest.
**RESUME.** Done.
