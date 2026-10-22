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
3 n 1e10 (item 9x0_s2d1e10_3n on s2dtest c96cb09e, 2026-10-22, measured): D 21.97 s (n 10, sd 0.43), P 14.99 s (n 8, sd 0.52); P - D over 8 ABBA pairs -6.92 s, 95 % CI [-7.57, -6.28]; A/A D2 - D +0.60 s (n 2). Phases D / P: init 13.23 / 4.53, bs 3.68 / 4.97, dm 4.11 / 4.96 s: the gain is all start-up; bs and dm are each ~1 s slower. 1 n (item 9x4_s2dtime1nb on s2fix 6b51c6c, 6 ABBA pairs each, measured): 1e9 D 9.07 / P 4.90 s, P - D -4.17 s [-4.95, -3.39]; 1e10 D 19.74 / P 20.38 s, P - D +0.63 s [-0.41, +1.68] (P init 5.87 vs 11.62, but dm 7.17 vs 1.85, recip 2.59 vs 0.90, bs 6.63 vs 5.70: one node's dm exchange goes through xshmem between 4 processes instead of shared memory in one). 3 n 1e11 (item 9x5_s2dfull3c, 4 ABBA pairs, all arms ECALC_NODE_GB=420; PG = P + DIST_CHUNKS=8 MN_T_CHUNK_MB=512 BS_SEED_CHUNK_MB=1024 COMM_SHMEM_POOL_MB=512 at cap 2^31, measured): D 83.36 s (sd 1.18), PG 84.32 s (sd 4.43; first run 90.95 s), PG - D +0.96 s [-5.54, +7.47]; phases D / PG: init 30.40 / 10.03, bs 24.80 / 36.95, dm 25.53 / 35.05, recip 9.85 / 13.11 s. 1.9323e11 at 3 n: P is refused (a PE's leaf top product exceeds one device's plane pool; decision Q75 / design batch split). Reading: one process per GPU saves start-up (-8.7 s at 3 n 1e10, -20 s at 3 n 1e11) but loses it in bs and dm at large sizes; no gain at 1e11; not a default candidate on aac7 as is.

S2fix (branch `s2fix`, 2026-10-22; plan outputs measured on the aac7 login node, gates 9x3b on 6b51c6c and 9x3c on dd8d821 PASS: P full size refused before launch, 1e9 D vs P digits identical at 1 / 2 / 3 n, pool from the plan at 1 / 2 / 3 n, 1 n P 1e10 runs (19.79 s); dd8d821 fixes review finding F1, the batch-tier check now reads the plane pool at the run's ECALC_PLANE_CAP): two P failures of the S2d runs. (1) 1 n 1e10 P died with rc 6 (`comm_xshmem: the DATA heap of PE 2 ... XSHMEM_HEAP_MB >= 4098 needed`): the pool rule returned 0 below 2 nodes, so a 1-node P plan printed no `plan pool` line and mnrun left the default heap. The rule now models the node's PE group (size = PEs per node) when `ECALC_PE_PER_GPU=1` and the transport is xshmem: 1 n 1e10 plans `XSHMEM_HEAP_MB=8448`, 1e9 `768`; default plans unchanged (md5 of the plan pool/fit lines equal to the old build at 1e11, 1 and 3 n). (2) 3 n 1.9323e11 PG died with rc 3 (`rns_mul_batch: the striped tier needs all 4 devices`): a PE's leaf top product (1.02e9 limbs = 2^30 points x 3 primes) exceeds one device's plane pool (3 x 2^29 limbs at cap 2^31, 3 x 2^28 at 2^30), so rns_mul_batch falls to the striped tier, which a one-device process cannot run; the planner did not model it. `MN_PLAN_ONLY` now prints `plan fit    batch tier ...` under P and refuses (`plan REFUSED`, mnrun rc 4 before launch) when the leaf's largest batch product does not fit; at 3 nodes cap 2^31 allows up to about 1.0e11 digits (1e11: 5.3e8 limbs, local tier OK, node 370.9 GB modelled with `DIST_CHUNKS=8 MN_T_CHUNK_MB=512 BS_SEED_CHUNK_MB=1024`; 1.2e11 refused), 1.9323e11 needs 6+ nodes or P is not usable. Two log artefacts fixed: the `BS_ARENA_ROOM ... is dropped` message came from the pool rule's room probe seeing no term range under P (arena 1726 GB instead of 197), and the `bs: arenas ... tails` figure summed uninitialised regions 1-3.

**Verdict.** Gate passed: merged off by default. S2d (2026-10-22): faster only at small sizes (start-up); equal at 3 n 1e11; full size needs Q75. Keep off by default.
**Open issues.** S2d. An omp team left PEs on device 0 (`vpe_rebind`) and per-vPE macros read inside omp regions: both fixed on `vpe-int`, not here. `vpe-int` must later merge main, re-enable the S1e bit and restore the `ECALC_VPE` README text.
**RESUME.** Merged from `ppg-int`; next: S2d paired timing, then S1e on `vpe-int`.
