# N7G — COMM_SHMEM_PX_THP and COMM_SHMEM_PX_PIN on the proxy mode: pairs and verdict

**Question.** Do huge pages (`COMM_SHMEM_PX_THP=1`) and pinned proxy pools (`COMM_SHMEM_PX_PIN=1`) shorten the 6-node SHMEM
proxy run against the default `COMM_OFI=0` path? Both switches default to 0.

**Change / branch.** Branch `n7g`, commit 006a80f (switches `COMM_SHMEM_PX_THP`, `COMM_SHMEM_PX_PIN`, default 0). Gates on
`n7g` passed before the pairs (m).

**Method.** Three interleaved pairs at 6 nodes, 3.8646e11 digits. Arm A: `COMM_OFI=0` (defaults). Arm B: A plus
`COMM_SHMEM_PX_THP=1 COMM_SHMEM_PX_PIN=1`. Difference is B − A, paired, 95 % CI from the t distribution with n = 3.
Driver and tables on aac7 (`n7g_pairs/tables.txt`); not copied here.

**Re-gate of item 36 on `n7f` (commit 76c8742).** Gates pass 11 of 11 at 1 and 2 nodes, each switch alone and all together;
2-node 1e10 compare identical (m). Summary on aac7 (`n7g_regate/summary.txt`).

**Results (m).**

| arm | runs | mean (s) | sd (s) | pair roffmax (s) |
|---|---|---|---|---|
| A: defaults (`COMM_OFI=0`) | 3 | 432.1 | 83.6 | 652 / 979 / 670 |
| B: + THP + PIN | 3 | 307.2 | 93.7 | 252 / 540 / 347 |

B − A = −124.9 s, 95 % CI [−460.6, +210.8], n = 3 pairs (m). Not significant.

**Page size (m).** The helper pools are 4 KB pages. Shared-memory huge pages are unavailable on this system.
`MADV_COLLAPSE` works on the pool and costs 0.29 s per GiB.

**Verdict.** Not significant. The default stays 0 for both switches. THP and PIN need more pairs before any adoption
decision (the three pairs have a CI wider than the effect).

**Open issues.** The run-to-run spread is large (sd 84 to 94 s on 3 runs); the open N7F item covers the same kind of spread. Four more
pairs of THP-only against the default are not run (aac7 budget, needs the user's word). The effect of PIN alone is not
separated from THP.

**RESUME.** Branch `n7g`, commit 006a80f plus the N7G docs commit. Done: results, RESULTS row, OPPORTUNITIES note, TASKS
rows. Next (not run): 4 more pairs, THP only against the default, at 6 nodes (queue via the aac7 runner, paired ABBA,
≤ 4 paired rounds per the project rule). Decision D35: merge defaults? No; both switches stay off.

## Item 37 (2026-10-15, gated): THP only, 4 pairs at 3.8646e11

Four interleaved pairs at 3.8646e11 digits. Arm A: default SHMEM. Arm B: `COMM_SHMEM_PX_THP=1` only (PIN left at 0).
Measured wall times: A mean 576.5 s (sd 155.5), B mean 288.8 s (sd 63.0). Paired B − A = −287.8 s, 95 % CI [−507, −68],
n = 4; B faster in 4 of 4 pairs (m).

Gates PASS: 1 and 2 nodes with THP, kill default on and off, comm_ofi 2-node gate, 6-node 1e10 compare identical (m).
Switch stays default 0 pending user decision D36.
