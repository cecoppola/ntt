# MEAS-P24 — four-prime product form (P24) for all multi-node products against the default, 2 nodes
**Question.** OPPORTUNITIES AUD3-6: time and device-memory cost of running P24 for every multi-node product (what `tools/gate.sh --p24` forces) at the standard 2-node size.
**Change / branch.** None (measurement only). Build `main` a6ba51d (SHMEM). Switch `ECALC_NP_AUTO_TERMS=1000000` (B), unset (A).
**Method.** Queue item `83_meas_p24` (user-approved 2026-10-12), one 2-node hold, driver `meas_abba.sh`: 4 rounds of A B B A (cap), 2 x 6.441e10 = 1.2882e11 digits, no digit writes, aac7 launch line from `tools/profile_load.py aac7 --line` (+ `COMM_XSTATS=1 MN_WAIT_STATS=1`). VERIFY OK and rc 0 on all 16 runs; B logs show `plane pool 0 for 4 primes`, A `... for 3 primes`. Digits not compared in these runs (the gate compares them at 10^9, `results/AUD3.md`).
**Results (measured).**

| statistic | value |
|---|---|
| A (default) totals | 138.71, 131.98, 133.83, 131.77, 133.14, 130.31, 138.00, 131.68 s (mean 133.7) |
| B (P24 forced) totals | 122.81, 122.93, 124.79, 125.04, 126.29, 125.99, 127.26, 123.33 s (mean 124.8) |
| round differences B - A | -12.47, -7.89, -5.58, -9.55 s |
| **mean difference B - A** | **-8.87 s, 95% CI [-13.48, -4.26], 4 pairs, p 0.009** (stop rule met) |

| stage (mean s) | A | B | B - A |
|---|---|---|---|
| binary splitting | 43.1 | 40.3 | -2.7 |
| distributed division multiply (dm) | 52.9 | 44.4 | -8.5 |
| init | about 34 | about 36.5 | about +2 |

| device memory, pool 0 at `[init]` (per node / per APU) | A | B | B - A |
|---|---|---|---|
| in use | 317.8 GB / 79.5 GB | 352.2 GB / 88.1 GB | **+34.4 GB / +8.6 GB** |
| of which planes | 103.1 / 25.77 GB | 120.3 / 30.07 GB | +17.2 / +4.3 GB |
| of which block pool | 214.7 / 53.69 GB | 231.9 / 57.98 GB | +17.2 / +4.3 GB |

Host RSS unchanged (HWM 28.9 GB). The peak during the run was not extracted; the init layout is the pool-0 size.

**Verdict.** P24 is faster, not equal, at 2 nodes: -8.9 s (6.6 %), from the division multiply (-8.5 s) and binary splitting (-2.7 s), at a cost of +8.6 GB per APU (+34.4 GB per node) of device memory at the init layout, twice the earlier modelled +4 GiB (planes and block pool both grow). Whether the target (which uses P24 from 32 nodes) is memory-bound decides the trade; the user decides adoption for the 2-node test form.
**Open issues.** The saving at the target size is not inferred; peak (not init) device memory not extracted; the 4-node three-prime gate form is unchanged.
**RESUME.** Raw: aac7 `~/meas/P24/{totals.tsv,stats.txt,log/}`. Nothing running.
