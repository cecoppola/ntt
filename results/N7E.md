# N7E — the N7D proxy-mode switches at 6 nodes: paired repeat (PX_SEND=2, PX_INIT=1) against comm_ofi

Scope: the N7E record (aac7 item n7e, 2026-10-14, 16:39 to 22:55 ET, commit 2f34f01, 6 nodes, 3.8646 × 10¹¹ digits): a paired ABBA
repeat of the proxy mode with `COMM_SHMEM_PX_SEND=2` and `COMM_SHMEM_PX_INIT=1` against `comm_ofi`, and the two switch-only
comparisons. Dated record; the code and the central docs win where they differ. Labels: m = measured, mod = modelled, a = assumed.

**Question.** Do the N7D switches `COMM_SHMEM_PX_SEND=2` and `COMM_SHMEM_PX_INIT=1` bring the proxy mode (`COMM_OFI=0`) to `comm_ofi`
speed at 6 nodes, and is the 1.43 × gap of results/N7D.md (config C plus N7D switches, 3 pairs) reproduced?

**Change / branch.** No code change. Branch `n7e` (worktree `../ntt-wt-n7e`), records only. Switches measured, all off by default
(unchanged): `COMM_SHMEM_PX_SEND` (default 1), `COMM_SHMEM_PX_INIT` (default 0).

**Method.** Launch line (all arms): `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto MN_T_CHUNK_MB=2048
COMM_SHMEM_ROUND_MB=1024 ECALC_MEM_GUARD_GB=6 ECALC_DEVICE_GB=444.0 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1`, plus the arm's
settings. Arms: A = `comm_ofi` (`COMM_OFI_VERBOSE=1`); B = `COMM_OFI=0 COMM_SHMEM_PX_SEND=2 COMM_SHMEM_PX_INIT=1`; I1S1 = `COMM_OFI=0
PX_SEND=1 PX_INIT=1`; I0S2 = `COMM_OFI=0 PX_SEND=2 PX_INIT=0`. Under an explicit `COMM_OFI=0` the N7D defaults of D31 apply to every
proxy arm (PX_NUMA and PX_KCOPY on) (assumed from TASKS/D31; the launch lines in the summary do not list them). Gate runs first at
1/2/4/6 nodes, 1e9 digits: all PASS, digits identical (summary). Totals per arm are the mean over runs with status ok; paired
differences are B − X per ABBA pair, mean with 95 % CI = mean ± t(n−1)·sd/√n. Overall: 83 of 83 recorded runs ok, 0 bad (summary);
1 planned run skipped for time. Per-arm wait columns are max over nodes (wbs / wdm / wrc: the bs, dm and reciprocal exchange waits).

**Results (6 n, 3.8646 × 10¹¹ digits; all times seconds; measured unless marked).**

| arm | n | total mean | sd | bs | dm | wbs | wdm | wrc | init |
|---|---|---|---|---|---|---|---|---|---|
| A comm_ofi | 29 | 191.0 | 15.1 | 76.6 | 78.0 | 145.0 | 164.6 | 56.9 | 32.3 |
| B PX_SEND=2, PX_INIT=1 | 35 | 348.3 | 111.1 | 130.3 | 175.3 | 322.2 | 401.2 | 189.7 | 35.6 |
| I1S1 PX_SEND=1, PX_INIT=1 | 4 | 421.7 | 58.5 | 161.2 | 212.1 | 364.1 | 441.2 | 246.8 | 39.4 |
| I0S2 PX_SEND=2, PX_INIT=0 | 3 | 313.3 | 43.5 | 115.3 | 158.5 | 279.7 | 342.1 | 192.8 | 35.2 |

Paired differences (negative = B faster):

| comparison | n pairs | total mean, 95 % CI | bs | dm | xw0 (exchange wait) | init |
|---|---|---|---|---|---|---|
| B − A (comm_ofi) | 28 | **+129.3 s, [+92.9, +165.7]**, excludes 0 | +43.6 [+30.6, +56.6] | +79.3 [+55.0, +103.5] | +338.8 [+251.2, +426.4] | +3.7 [+2.7, +4.6] |
| B − I1S1 (PX_SEND 2 vs 1) | 4 | +69.9 [−217.8, +357.6], n.s. | +13.3 [−100.7, +127.3] | +60.1 [−135.0, +255.1] | +154.0 [−526.9, +834.9] | −4.6 [−19.8, +10.5] |
| B − I0S2 (PX_INIT 1 vs 0) | 3 | +107.2 [−50.5, +264.9], n.s. | +50.1 [−52.6, +152.8] | +57.8 [−5.2, +120.8] | +250.8 [+82.1, +419.4], excludes 0 | −1.3 [−2.7, +0.1], n.s. |

Derived (measured): B / A = 1.68 × (paired, 190.8 s comm_ofi mean). Earlier config C with the N7D switches (results/N7D.md, 3 pairs):
274.8 s, 1.43 × comm_ofi, +83.2 s [+37.7, +128.7]. Spread of B is 111.1 s sd against 15.1 s for A.

**Verdict.** Not adopted; the defaults stay `COMM_SHMEM_PX_SEND=1`, `COMM_SHMEM_PX_INIT=0`. The gap to `comm_ofi` at 6 nodes is
+129.3 s (1.68 ×), wider than the 1.43 × (+83.2 s) of N7D: the N7D gap closure is **not reproduced**. The PX_SEND=2 vs 1 and
PX_INIT=1 vs 0 point estimates are not faster and their CIs include 0 (PX_SEND: n = 4; PX_INIT: n = 3). PX_INIT saves about
1.3 s of init (95 % CI [−2.7, +0.1], n.s.), so the pre-fault switch does not help either. The exchange waits remain the cost:
B's xw0 is +338.8 s over A (CI excludes 0). Needs more pairs to resolve PX_SEND and PX_INIT; a larger run is not justified by these data.

**Open issues.** (1) The source of the 111 s sd of the B arm (run-to-run spread on the proxy path, not located). (2) PX_SEND=2 and
PX_INIT=1 at more pairs (total CI half-widths 288 s at n = 4 for PX_SEND, 157 s at n = 3 for PX_INIT; measured). (3) Whether B carries the D31 defaults PX_NUMA and PX_KCOPY exactly as the N7D
config C did (assumed, see Method). (4) One planned run was skipped for time.

**RESUME.** Branch `n7e` off `main` 0a9d778; this file and the RESULTS, OPPORTUNITIES and TASKS rows are the record. No code changed.
Raw summary and tables stay on the cluster (`~/day/n7e/`), not in the repository. Next step, if the user schedules it: a paired
ABBA of B against A with more pairs, only on the user's word (aac7 budget, CLAUDE.md).
