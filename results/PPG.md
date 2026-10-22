# PPG — TARGET1 stage S2, one process per GPU
**Question.** Does one process per GPU (the Cray layout, xshmem K=1) give the same digits as the node-process default, and what does it cost or save?
**Change / branch.** Branch `ppg-int` (head 31d566a, squash-merged to main). Switch `ECALC_PE_PER_GPU` (default 0, off). Also carried: the S1 vPE stages b-e (`ECALC_VPE`); thread-mode full runs abort with `stages e not merged` (`VPE_STAGES_MERGED` lacks S1e: the S1e flow is not end-to-end correct); component tests still run.
**Method.** aac7, 3 healthy nodes, item 9v8_ppgint3i on 31d566a (SUCCESS 2026-10-21 23:46 EDT). Component tests: `t_vpe` host and stages with and without `--ppg`; `t_vpe all --ppg` at 1/2/3 n; `t_out --vpe` and `--ppg`; `t_bs`; `t_mn_grid --vpe` in process and thread mode. Guard: thread-mode e8 abort check. `mnaccept` unit set. Off path v0off1/2/3. End to end off vs on (`ECALC_PE_PER_GPU=1`), digits compared.
**Results.** All (measured).

| check | result |
|---|---|
| `t_vpe all --ppg` at 1 / 2 / 3 n | 4 / 8 / 12 PASS |
| thread-mode e8 abort | rc 3, `stages e not merged` |
| off path v0off1/2/3 | PASS, digits identical |
| mnaccept unit set | PASS |

| size | nodes | off (s) | on (s) | digits |
|---|---|---|---|---|
| 1e9 | 1 | 9.73 | 8.94 | identical |
| 1e9 | 3 | 14.27 | 5.42 | identical |
| 1e10 | 3 | 32.83 | 24.68 | identical |

One run each: not a speed claim. Earlier gate 9v5 (4e32c99): same result plus XRD 3 n 1.9323e11 VERIFY OK 160.1 s (m).
Bugs found and fixed on the way: `rns_init` set up all 4 devices per process (OOM); the seed copy device was `region % nd` (device 0 for every PE, SEGV); the batch tier chose striped over 4 devices on a one-device process; the vPE exchange worker cost about 35x the poll mode, so poll is the default under vPE and process mode.

Launcher fixes (branch `fitppg`, gates 9w2x/9w2w on f212a9e/6aab0bc, 2026-10-22, measured): mnrun's FIT pre-check planned `ECALC_PE_PER_GPU=1` runs without the per-node PE count, so the plan died and a run that does not fit launched (9w2_s2dfeas3: P 1.9323e11 3 n killed after 16 s); now refused before launch (no node time). On one node, P now picks `COMM_TRANSPORT=xshmem` as for more than one process. Gate 9w2w: P full REFUSED before launch; 1e9 D vs P digits identical at 1 / 2 / 3 n (single runs D / P: 10.86 / 8.28, 15.21 / 6.64, 16.07 / 6.41 s).

Pool plan (branch `poolppg`, gate 9w2u on 5b4ec8a, 2026-10-22, measured): the launcher's pool-sizing plan had the same gap, so P runs kept `COMM_SHMEM_POOL_MB=8192` and no `XSHMEM_HEAP_MB` from the plan; now the plan sizes them at 2 / 3 n (pool 512 MiB from the plan; digits identical at 1 / 2 / 3 n). On one node the engine's pool rule still returns no pool line (TASKS Proposed).

## S2d timing
3 n 1e10 (item 9x0_s2d1e10_3n on s2dtest c96cb09e, 2026-10-22, measured): D 21.97 s (n 10, sd 0.43), P 14.99 s (n 8, sd 0.52); P - D over 8 ABBA pairs -6.92 s, 95 % CI [-7.57, -6.28]; A/A D2 - D +0.60 s (n 2). Phases D / P: init 13.23 / 4.53, bs 3.68 / 4.97, dm 4.11 / 4.96 s: the gain is all start-up; bs and dm are each ~1 s slower. Pending: 1 n pairs and 3 n 1.9323e11 (P needs a smaller layout to fit the real ~420 GB per node: arm PG, item 9x1_s2dfull3b).

**Verdict.** Gate passed: merge as off by default. Adoption needs S2d.
**Open issues.** S2d. An omp team left PEs on device 0 (`vpe_rebind`) and per-vPE macros read inside omp regions: both fixed on `vpe-int`, not here. `vpe-int` must later merge main, re-enable the S1e bit and restore the `ECALC_VPE` README text.
**RESUME.** Merged from `ppg-int`; next: S2d paired timing, then S1e on `vpe-int`.
