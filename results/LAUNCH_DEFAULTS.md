# LAUNCH_DEFAULTS — launch-line decisions and defaults adopted

Owner of items B01, L1, T08, ADOPT2, DEF, FIT, S47, AAC7OPT, A2EDGE (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; originals: `archive:results_merged_2026-10-19/`. Labels: m measured, mod modelled, a assumed. Times Eastern.

## Topic summary

What is current (see `ecalc/README.md` Switches table, which wins on any difference):
- Launch lines: the build and launch lines of both paths and the sources of the aac7 and target lines are owned by `PLAN.md` section 3 (not repeated here); `ECALC_SEGV_TRACE=1` is set by `gate.sh`. The B01/L1 line (`COMM_LAYER_INTER2`, VSLOT and cache-fit switches) is history: X2 was removed 2026-10-23, the others are built in (2026-10-12). Fastest aac7 settings by size: §AAC7OPT.
- K2 + K3 (`DBIG_ADDSUB3`, `DBIG_MODQ_MONT`) default 1 (T08); `=0` is the fallback. K4 `RNS_CRT_SUB` stays off.
- `P24_CRT_SCAN=1` (D24) and `NEWTON_CHAIN_GUARD=1` (D25) are code defaults (ADOPT2); the guard costs +0.25 s [-1.00, +1.50] (n.s.) at 1 node.
- P24 (four primes, 24 digits per point) is the default for every multi-node product (D18, DEF); `ECALC_NP_MN3=1` kept the three-prime path as a fallback (removed 2026-10-19, D56). P24 costs +17.18 GB planes per node at 6.441 × 10¹⁰ digits per node (m).
- `DM_MN_WRAP` is now AUTOMATIC (D26, FIT): unset = 1 only when the device peak with the wrap is at most `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB`; a set value wins. DEF's "DM_MN_WRAP default stays 0" and B01/L1's rank-0 figures are superseded by FIT. (D22 had made 1 the default in between; the automatic rule replaced it.)
- Memory fit before every launch (FIT): planner/runs print `plan fit` / `memory fit:`; a run that does not fit is refused (`plan REFUSED`, exit 8; `mnrun.sh` exit 4; `gate.sh` FAIL). Current target figure: 3.82 × 10¹³ digits on 576 nodes with the defaults (`MN_NONPOW2_CAP` auto = on): device 407.41 GB vs the 373.44 GB edge, margin -33.97 GB, REFUSED (planner, m, 2026-10-25; wrap off); with `MN_NONPOW2_CAP=0`: 358.96 GB, +14.48 GB (the 2026-10-19 figure; with wrap 384.73, refused). Cause: NPCAP's larger planes (see the table note). `ECALC_DEVICE_GB` default 373.44; `profiles/aac7.conf` 444.0 (m).
- S47 (4-node re-confirm of T2048 and X2) was inconclusive; both stay on the aac7 line on their 10-node evidence (`results/SESSIONS.md`, S31).
- Open: P24 at non-power-of-two groups below 32 nodes not gated; DEF's 6-node `total` difference for the wrap not significant (n.s.); FIT figures are layout values, not a target measurement.

## B01 -- launch-line decisions of 2026-10-11 (branch b01 from main bc80001)

History. Put `MN_T_CHUNK_MB=2048` and the X2 exchange on the target line (X2 removed 2026-10-23); modelled 576-node memory at 3.82e13 with them was 369.1 GB rank 0 (margin 4.3 GB, mod), which L1 improved. Superseded by FIT.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/LAUNCH_DEFAULTS.md`.)

## L1 -- COMM_LAYER_VSLOT_RELEASE=1 on the launch lines (user, 2026-10-11)

History. Slot release (now built in) cut the comm_ofi pool 4352 to 2304 MiB per APU and the 576-node device figure by 8.59 GB (m, layout), restoring a 12.9 GB margin at 3.82e13 (mod). Superseded by FIT.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/LAUNCH_DEFAULTS.md`.)

## DEF — P24 as the multi-node default (D18) and DM_MN_WRAP in the memory model + 6-node A/B (D19 b)

Question: D18 makes P24 the default for every multi-node product (three-prime path stays selectable); D19 (b): count `DM_MN_WRAP`'s extra block-pool memory in `dm_layout`/`mem_model.py` and time `DM_MN_WRAP=0` vs `=1` at 6 nodes with the new default. Branch `def` (from main d1d19bf).
Verdict: D18 (P24 default) merged. `DM_MN_WRAP=1` saves 6.25 s of the division (p 0.032, 4 rounds) but `total` is n.s.; costs +5.4 GB per APU at the division's peak. Decision went to the user (became D22, then automatic D26). Open: `BS_LAYOUT_ONLY` reads `ECALC_NP` once per process (lay out multi-node points with `ECALC_NP=auto`); P24 at non-power-of-two groups below 32 nodes not gated; 4 more rounds could settle `total`; the wrap term was checked against one peak (5.37 vs 5.73 GB, 6 nodes) and P1's 2-node growth (4.3 vs 8.59 modelled). Raw: aac7 `<cluster work dir>/def/`.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/LAUNCH_DEFAULTS.md`.)

## FIT — the memory fit before every launch, and DM_MN_WRAP automatic

Question (user decision D26, 2026-10-13): make the wrap automatic, and know before launch how well the memory fits. The planner (`MN_PLAN_ONLY`) only checked the 480 GB node budget: at 3.82e13 on 576 nodes with `DM_MN_WRAP=1` the model gave 375.1 GB device vs the 373.44 GB edge and the plan still said OK (`archive:results_merged_2026-10-23/NIGHT_RUNS.md` §NIGHT, section 7; NIGHT's 375.1/345.0 were `mem_model.dev_max`, which omits the 9.66 GB comm_ofi pools). Branch `fit`; digits unchanged.

Change:
- `binsplit.c binsplit_fit` (source of truth): device peak per node = max(dm phase: planes + arena + comm_ofi pools + 4 v-slots; bs phase: same + VMM bs growth 6.0 GB + tree-top v-slots), per APU = /4, vs `ECALC_DEVICE_GB`; node = planes + arena + bs growth + host + v-slots vs `ECALC_NODE_GB`. Printed by `MN_PLAN_ONLY` (`plan fit`; refusal `plan REFUSED ...: the memory does not fit`, exit 8), every run (`memory fit:` on rank 0), `BS_LAYOUT_ONLY` (`fit:`). comm_ofi pools count as device memory (`COMM_OFI_POOL=host` moves them out).
- Switches: `ECALC_DEVICE_GB` default 373.44 (target edge, 93.36 GB per APU, m, `results/TARGET.md §TGTBENCH2`); `ECALC_DEVICE_MARGIN_GB` default 5.0 (the tighter of CAP17's two bars). `profiles/aac7.conf`: `ECALC_DEVICE_GB=444.0` (m: aac7 SPX t_edge, VMM 444.0 GB committed, hipMalloc about 448.0; `archive:RESULTS_ARCHIVE_1-114.md` section 107; the lower, graceful one taken); `profiles/target.conf.template`: 373.44.
- `DM_MN_WRAP` automatic: unset, the wrap is taken only when device-with-wrap ≤ `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB`; a set value (0/1/k) wins with a warning if it does not fit; all ranks decide alike. Config line `dm_mn_wrap:<v>(fit|set|default)`; size 1 does not use the wrap.
- Launchers: `tools/plan_values.py --fit <digits> <nodes>` (exit 1 = does not fit; the device comm pools are always counted since Q103, 2026-10-25); `ecalc/mnrun.sh` (refuses with exit 4 before srun; `MNRUN_FIT=0` skips); `tools/gate.sh` (`fit.txt`, FAIL if not fit; `GATE_FIT=0` skips); `target_kit.sh` plan stage. `mem_model.py --check-c` ports the decision. Planner with fit 0.73 s vs 0.58 s before (m, MN_PLAN_ONLY 3.82e13:576, login node).

Results (planner on the aac7 login node; figures from the C layout, mod). GB:

| run | profile | device per node (per APU) / limit | margin | node vs 480 | node margin | DM_MN_WRAP |
|---|---|---|---|---|---|---|
| 3.82e13 on 576, defaults (NPCAP auto on) | target | 407.41 (101.85) / 373.44 | -33.97 | 443.49 | +36.51 | 0 (fit): with it 433.18; REFUSED |
| 3.82e13 on 576, `MN_NONPOW2_CAP=0` (the 2026-10-19 figure) | target | 358.96 (89.74) / 373.44 | +14.48 | 395.04 | +84.96 | 0 (fit): with it 384.73 > 368.44 |
| same, `DM_MN_WRAP=1` set | target | 384.73 / 373.44 | −11.29 | 421.62 | +58.38 | 1 (set): WARNING, REFUSED |
| 6.441e10 on 1 | aac7 | 290.09 / 444.00 | +153.91 | 316.44 | +163.56 | not used at size 1 |
| 1.2882e11 on 2 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit): without 368.47 |
| 2.5764e11 on 4 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit) |
| 3.8646e11 on 6 | aac7 | 374.22 / 444.00 | +69.78 | 409.24 | +70.76 | 1 (fit): without 348.45 |

- Why 358.96 became 407.41 (bisect 2026-10-25, planner on the build caches and login builds, `ECALC_NP=auto MN_T_CHUNK_MB=2048 ECALC_DEVICE_GB=373.44`, xshmem; m, planner): 36a4c38 and d330ef3 (10-13, 10-19) 349.29 GB without the comm pools (planes 120.88); 463f87d (10-21) 394.52 without comm pools (planes 162.22, v-slots 11.56); from 10-21 the xshmem default counts the comm pools (Q73): +9.66 with the plain plane, +12.88 with NPCAP; today 407.41. The step is `MN_NONPOW2_CAP` auto (Q58, `results/DIVISION.md §NPCAP`: plane memory +41.3 GB, as this planner shows: +41.34); the 10-19 figure was never re-run after it; `BS_ARENA_ROOM` 0.14 vs 0.16 changes nothing here (arena 214.75 either way). A real need, not a planner over-count. Per-term at 3.82e13 on 576 with `MN_NONPOW2_CAP=0` (peak is the bs phase; dm phase 352.96): planes 120.88 + arena 214.75 + comm_ofi pools 9.66 + bs growth 6.00 + v-slots 7.67 GB; per APU arena 53.69 (dm need 51.73: reciprocal set 38.53, division 37.45; tree 50.93), plane pools 30.22; wrap term 7.64 GB per APU.
- `--check-c` on six `BS_LAYOUT_ONLY` points with the launch-line settings (`COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=2048`, `ECALC_DEVICE_GB=444`): 0 terms inexact, wrap decision equal everywhere (m); also exact after rebasing on main (rebuilt with `make -B SHMEM_CRAY=1`, gates not rerun after the rebase).
- Gates (aac7, commit 54f62a1, m): `gate.sh --build --nodes 2` PASS (VERIFY OK, digits identical, P24; fit 137.61/444.00 GB; `dm_mn_wrap:0(fit)` at 1e9); `--nodes 1` PASS (fit 120.44/444.00); 6 nodes 3.8646e11 via `mnrun.sh` (no digit output): rc 0, VERIFY OK on all 7 lines, `dm_mn_wrap:1(fit)`, ecalc total 238.5 s (one run, not a timing claim). The first attempt failed at doccheck because an untracked stand-in `profiles/target.conf` was in the worktree.

Verdict: done as decided (D26); the fit is computed before every launch, a run that does not fit is refused, the wrap is automatic. The target at 3.82e13 on 576 fitted with the wrap off (+14.48 GB, planner) before NPCAP's default; with `MN_NONPOW2_CAP` auto it is refused (-33.97 GB, 2026-10-25). Merged.
Open: the device figure is the layout's, not a target measurement (bs growth 6.0 GB measured at size 1 on aac7; comm_ofi pools depend on `COMM_OFI_PLAN_CXI`); `mem_model.py --check-c` without `COMM_SHMEM_ROUND_MB=1024` differs in the comm_ofi pools at 6 and 576 nodes (-11.6 % / -19.5 %; exact with the launch lines' 1024); `mem_model.py dev_max` omits the comm_ofi pools (345.0 vs C's 358.96 GB); aac7 without its profile gets the 373.44 GB edge (stricter, never looser).

## S47 - exchange settings re-confirm at 4 nodes (aac7, batch 2026-10-11, track T2)

Finished 2026-10-11 08:56; **verdict INCONCLUSIVE at 4 nodes**.
Question: on the aac7 line (`e16_headline.sh`, 4 nodes, 6.441e10 digits per node = 2.5764e11 total, no digit output): (i) `MN_T_CHUNK_MB` 1024 (A) vs 2048 (B; aac7 line since 2026-10-08, S22 D2: 24 rounds, −42.3 s at 10 nodes); (ii) X2 off (A) vs on (B; since 2026-10-09, S31: −31.7 s at 10 nodes). `COMM_LAYER_VSLOT_SHARE=1` kept in both arms.
- Verdict: inconclusive; point estimates (+7.0 s, −2.7 s) well inside the noise (±17, ±10 s). Both settings stay on the aac7 line (evidence: the 10-node S22/S31 runs). To resolve at 4 nodes: remove the cause of the bimodality (per-host stage times, nodechk, fixed node set via `-w`); or about 30+ pairs for (i) (about 3.5 h per comparison, mod); or a paired design with A and B back to back on the same nodes using stage-level times. Rule (user): adopt/keep only on a CI that excludes 0 with identical digits. Raw: aac7 `<cluster work dir>/s47/run/`.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/LAUNCH_DEFAULTS.md`.)

## AAC7OPT — the fastest settings on aac7 (main 815b7a0 = 208ae18 code, 2026-10-25 13:53-14:31 ET)

Question (user 2026-10-25): the optimal ecalc settings on aac7. Items b5g_opt1n, b5h_opt1n2 (4 NIC-free nodes, 1 n), b5o_opt and
b5p_tchunk (the two networked nodes); one build each of path 1 (`XSHMEM=ofi`) and path 2 (`XSHMEM=cray`); base env = the aac7 profile line
(`ECALC_NODE_GB=420 ECALC_DEVICE_GB=444.0 MN_T_CHUNK_MB=2048 ECALC_MEM_GUARD_GB=6`); arms in Latin / ABBA order, paired by round;
`ecalc total` of PE 0; no digit file; every run VERIFY OK except the `POOL_LOG=30` arm (rc 6 at start). Labels: all m.

| question | size | pairs | result (arm - base, 95 % CI) |
|---|---|---|---|
| D (default layout) vs P (`ECALC_PE_PER_GPU=1`, path 1) | 1 n 6.6e10 | 4 | P +9.62 s [+4.98, +14.26] (D 74.2, P 83.8 s; dm 16.9 vs 28.4 s) |
| `ECALC_NP=3` / `=4` vs auto | 1 n 6.6e10, D | 8 | -0.19 [-1.23, +0.85] / +7.51 [+4.89, +10.13] (auto = 3 primes here) |
| `BS_SEED_CHUNK_MB=2048` | 1 n 6.6e10, D | 8 | +0.25 [-0.66, +1.17] |
| `ECALC_BG_THREADS=96` / `=24` (default 48) | 1 n 6.6e10, D | 8 | +1.12 [-2.42, +4.65] / -0.39 [-2.16, +1.37] |
| `POOL_LOG=30` | 1 n 6.6e10, D | - | fails at start (rc 6) |
| P vs D | 2 n 5e10 | 3 | P -14.71 s [-21.13, -8.30] (D 54.4, P 39.7 s, -27 %) |
| path 2 (`XSHMEM=cray`, P, `XSHMEM_LANES=2`) vs P (path 1) | 2 n 5e10 | 3 | +13.85 s [+10.76, +16.94] (53.5 s; vs D -0.86 [-8.78, +7.06]) |
| `MN_T_CHUNK_MB=1024` vs 2048 under P | 2 n 5e10 | 3 + 6 | +0.62 s over the 9 pairs (b5o -2.36 [-10.1, +5.4], b5p +2.11 [-1.03, +5.25]) |
| `XSHMEM_ROUND_MB=512` vs 128 under P | 2 n 5e10 | 3 | -0.65 [-11.15, +9.85] |

Capacity (planner, `plan_values.py --fit`, same base env): P fits 2 n up to 5e10 (node 412.5 GB of 420; 5.5e10 refused at 425.8 GB, the
host terms of four processes per node + Q110's staging); D fits 2 n to about 1.2e11 (1.32e11 refused by 1.7 GB); 1 n 6.6e10 fits both.

Verdict. On aac7: 1 node at the target's per-node share (6.6e10) runs fastest with the defaults (layout D, three primes); P wins at 1 n only
at smaller sizes (3.3e10: -5.5 s, §PEER of `results/SINGLE_NODE.md`). On 2 nodes P on the default build is fastest (-27 %) wherever it
fits (to 5e10); above that, D. Path 2 is the target's shape, not aac7's fastest. No other setting tested moves the time beyond noise: the
profile stays as it is (`MN_T_CHUNK_MB=2048` kept, no evidence either way under P). aac7's slow mode (one run in three, dm +4 s) is the
noise floor of these pairs (`results/NODE_SLOWMODE.md §BM`).

Sections moved to `archive:results_merged_2026-10-24/LAUNCH_DEFAULTS.md` (2026-10-24 cleanup): T08; ADOPT2.

## P2JUMP — path 2's device need at 64 vs 576 nodes (2026-10-25)

Question (TASKS "Path 2 at the target's scale"): at the same per-node share 1.736e10 (1e13 on 576 n) the planner gives path 2 (`ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`) 380.23 GB per device-node at 576 n against a much smaller need at 64 n. Method (all modelled): `tools/plan_values.py --fit` of an xshmem build of main 1739cd4's planner (ECALC_DEVICE_GB 373.44, ECALC_NODE_GB 414), digits = 1.736e10 x nodes, the terms read from its `plan fit` lines; no node touched. This binary reproduces 380.23 GB at 576 n exactly but gives 276.13 GB at 64 n, not the 293.31 GB quoted in TASKS (cause of the 17 GB difference not found; the 64 n value is the same at 2-64 n and at 128, 256 and 512 n).

**Sweep, device GB per node, path 2 (planes + arena + comm_ofi pools + v-slots; wrap = DM_MN_WRAP's figure when the rule turns it on):**

| nodes | device | planes | arena | ofi pools | v-slots | note |
|---|---|---|---|---|---|---|
| 1 | 178.42 | 103.70 | 68.72 | 0 | 0 | |
| 2-64, 128, 256, 512 (powers of two) | 276.13 | 120.88 | 146.03 | 3.22 | 0 | the same at every power of two |
| 96, 192, 384 | 340.23 | 120.88 | 120.26 | 47.24 | 45.85 | non-power-of-two |
| 448 | 327.28 | 120.88 | 120.26 | 40.80 | 39.34 | |
| 288, 576 | 371.64 (380.23 with the wrap) | 120.88 | 120.26 | 63.35 | 61.15 | node 467 GB (budget 414) |
| path 1 (layout D) 64 / 576 | 232.10 / 238.97 | 120.88 | 103.08 / 96.64 | 2.15 / 9.66 | 0 / 5.79 | same share, `MN_NONPOW2_CAP` has no effect below 4.5e10 per node |

**Cause.** The jump is not the node count itself but "is the group a power of two". A power-of-two group uses `ntt_dist`'s transform, which takes its exchange scratch from the block pool: v-slots 0 (`rns_mul_dist_mn_vslot` returns 0). 576 nodes = 2304 PEs = 9 x 2^8 (and the tree's groups 192 and 576) are not powers of two, so every product uses the general map, whose v-exchange slots (two per APU, 2 x (A + max(A, B)), hipMalloc'd beside the arena, kept to the run's end) are resident: 1.92 GB per APU in layout D (K = 8 chunks) but 15.29 GB per APU under one process per GPU, because `DIST_CHUNKS` defaults to 1 under vPE (ntt_dist.c, rns_dist.c: "one exchange per transform") and the slot size is proportional to 1/K (15.29 / 1.92 = 7.97; the Python model with K = 1 gives 15.31). The v-slots set the comm_ofi pools too (`XSHMEM_HEAP_MB` 15104 = 256 MiB + 2 x 7291 MiB slot, per APU) and the planner books them twice: 61.15 + 63.35 GB. The arena shrinks by 25.8 GB at the same time (general-map grid), so the net is +95.5 GB (371.64 vs 276.13) and +104.1 with the wrap. 96 = 3 x 32 is already +64 GB; 448 = 7 x 64 is +51 GB. 576 = 9 x 64 is the worst because the top group and the 192 group both stay general-map; nothing here is the 576-node scale itself.

**Fix options (modelled, path 2 at 1.736e10 per node, 576 n):**

| setting | device | node | v-slots | ofi pools |
|---|---|---|---|---|
| default (`DIST_CHUNKS` unset = 1) | 380.23 refused | 467.17 | 61.15 | 63.35 |
| `DIST_CHUNKS=2` | 318.56 fits | 414.09 | 30.62 | 32.21 |
| `DIST_CHUNKS=4` | 288.22 | 383.76 | 15.32 | 17.18 |
| `DIST_CHUNKS=8` | 273.06 (= the 64 n figure less 3) | 368.59 | 7.67 | 9.66 |
| `DIST_CHUNKS=16` | 264.94 | 360.47 | 3.84 | 5.37 |

**A3 re-run on the built cand3 planner (2026-10-28, `ECALC_PE_PER_GPU=1 SLURM_NTASKS_PER_NODE=4 XSHMEM_LANES=2 ECALC_FIT_PER_APU=1`, target limits device 373.44 / node 487, 576 n, planner output = mod; the device / node figures reproduce the table above to the 0.01 GB).** GB per node (per busiest APU against 93.36); "unset" is `DIST_CHUNKS` not set (= 1 under vPE):

| digits | `DIST_CHUNKS` | device (per APU) | node | verdict, per-APU margin |
|---|---|---|---|---|
| 4.608e12 (8e9 per node) | unset = 1 / 2 / 4 / 8 | 328.69 (82.17) / 267.02 / 236.68 / 221.52 | 406.32 / 344.65 / 314.31 / 299.15 | fits, +11.19 / +26.61 / +34.19 / +37.98 |
| 1e13 (1.74e10 per node) | unset = 1 | 371.64 (92.91) | 467.17 | fits by +0.45 per APU (+1.80 node-mean); the wrap (380.23) is off by the auto rule, so the older "refused" figure was against the old 414 GB node budget |
| 1e13 | 2 / 4 / 8 | 318.56 (79.64) / 288.22 (72.06) / 273.06 (68.26) | 414.09 / 383.76 / 368.59 | fits, +13.72 / +21.30 / +25.10 (wrap on) |
| 3e13 (5.2e10 per node) | unset / 1 / 2 / 4 / 8 | 611.02 (152.75) / 611.02 / 519.01 / 473.58 / 450.29 (112.57) | 741.94 / 741.94 / 649.93 / 604.50 / 581.21 | REFUSED at every `DIST_CHUNKS` up to 8 (-59.39 ... -19.21 per APU; node over 487 too) |
| path 1 D reference: 4.608e12 / 1e13 | default | 135.54 (40.33 busiest) / 238.97 (74.24) | 160.54 / 268.12 | fits, +53.03 / +19.12 |

Reading: the 8e9-per-node campaign shape fits path 2 with 11 GB per APU to spare even at `DIST_CHUNKS` unset; 1e13 fits only barely unset and comfortably at 2 or more; 3e13 does not fit path 2 at any slicing (the tree term, see the 6.632e10 note below). `DIST_CHUNKS` > 1 under vPE is still not run (see Digit safety). `DIST_CHUNKS=16` gives 264.94 GB (66.23 per APU, +27.13) at 1e13 and 439.22 (109.80, -16.44, REFUSED) at 3e13. `XSHMEM_LANES` 1 / 2 / 4 does not change any planner figure (checked at 1e13 and 3e13, `DIST_CHUNKS` 1 and 16).

`MN_NONPOW2_CAP=0` changes nothing here (it is off by auto below 4.5e10 per node). `COMM_ALLTOALLV_DEPTH=1` changes nothing in the planner. `MN_GROUPS` cannot help: the top group must be all 576 nodes. A power-of-two node count (512) avoids it (276.13) but is not the target. Digit safety: `DIST_CHUNKS` only re-slices the rows of one exchange (digits were identical for 4 -> 8 on layout D, README), but under vPE K > 1 has not been run, the K = 1 default was chosen for message size (64 KiB at K = 8), and the effect on time is unmeasured: it needs the gate at 3 nodes (12 PEs, general map) with `ECALC_PE_PER_GPU=1` and an ABBA at 3 n before anything is adopted. Smallest change that fits: `DIST_CHUNKS=2` (node 414.09 against 480 GB real budget), or 4 (margin +85 GB device).

**The 6.632e10 per node share (earlier note), same binary:** device with `DIST_CHUNKS` unset, `MN_NONPOW2_CAP` auto (on from 4.5e10): 362.03 (2 n), 430.75 (8 n), 456.52 (64 n), 671.15 GB (576 n; planes 162.22, v-slots 91.69); with `MN_NONPOW2_CAP=0` 576 n is 560.62 and with also `DIST_CHUNKS=8` 453.45 GB (v-slots 7.67). The growth from 2 to 64 n at that share is the tree term of path 2 (arena 232 / 301 / 326 GB at 2 / 8 / 64 n, tree 44.95 / 63.74 / 70.02 per APU) while layout D stays at 360.95 GB: path 2's plan runs the tree over the nodes (`plan pe` line: the groups below a node are not modelled until the PE-level products exist), so each deeper level adds a bigger product per PE; the dm need (55.96 GB per APU) is the same in both. No K or cap setting removes that: path 2 needs a PE-level tree model (and then a smaller real footprint) to fit 6.6e10 per node; values here differ by 8-9 GB from the earlier note's (447.93 / 473.70 / 517.67), a planner or settings difference not traced.

## A2EDGE — record memory edge per APU at 3e13 and 3.82e13 on 576 nodes (2026-10-28, planner, mod)

Question (overnight lane L3, A2): which setting has the largest margin under the per-APU edge 93.36 GB (assumed edge from the target: the `hipMalloc` limit per APU, `ECALC_DEVICE_GB` 373.44 / 4) and the node limits, given that the target measured 2.7-6.5 GB more than the planner per APU (so a margin under 6.5 GB is not safe). Method (all planner output, mod): cand3 build (path 1, layout D), `MN_PLAN_ONLY=<digits>:576`, `ECALC_FIT_PER_APU=1 ECALC_DEVICE_GB=373.44 ECALC_NODE_GB=487`, `COMM_TRANSPORT=xshmem`, one run per cell (about 3 s each, 112 + 48 runs). All five switches exist (`ecalc/README.md`); `DIST_CHUNKS` unset is 8 on path 1 (README), so the requested {1, 2} are both coarser than the default; 16 was added.

**Result 1, the planner's per-APU peak is the busiest APU (region 0) unless `BS_MDEV_LOGL` balances it.** Per-APU max / margin to 93.36 (GB), `MN_NONPOW2_CAP=0`, `MN_T_CHUNK_MB` 512 or 1024 (no effect at `DIST_CHUNKS` 8 / 16), `DM_MN_WRAP` auto:

| digits | `BS_MDEV_LOGL` | `DIST_CHUNKS` 8 (default) | `DIST_CHUNKS` 16 |
|---|---|---|---|
| 3e13 | unset | 89.74 / +3.62 (device 350.37, node 386.45; wrap on) | 88.13 / +5.23 |
| 3e13 | 29 .. 24 (all equal) | 85.44 / +7.92 (device 341.78, node 377.86; wrap on) | 83.83 / +9.53 (wrap on); with `DM_MN_WRAP=0` set 75.24 / +18.12 (device 300.97) |
| 3.82e13 | unset | 89.74 / +3.62 (device 354.66, node 390.75) | 88.13 / +5.23 |
| 3.82e13 | 29 | 89.74 / +3.62 (not yet balanced: device 352.51) | 88.13 / +5.23 (device 346.07) |
| 3.82e13 | 28 .. 24 (all equal) | 87.59 / +5.77 (device 350.37, node 386.45) | **85.98 / +7.38** (device 343.92, node 380.01) |

- `BS_MDEV_LOGL` 28 or lower balances the four APUs at 3.82e13 (busiest = mean 87.59); 29, which balances at 1e11 on one node (`results/MEMORY.md` §B4) and at 3e13 here, leaves region 0 at 89.74 at 3.82e13. Lowering below 28 changes nothing in the planner (the time cost of more mdev levels is unmeasured).
- `MN_NONPOW2_CAP` auto (on from 4.5e10 per node) is REFUSED at 3.82e13 in every cell (best 99.70 per APU, -6.34, with `BS_MDEV_LOGL=28`) and at 3e13 fits only with `BS_MDEV_LOGL` 28/29 and no coarsening (91.11 / +2.25): keep `MN_NONPOW2_CAP=0`.
- `DIST_CHUNKS` 1 and 2 are worse than the default 8 in every cell and REFUSED at 3.82e13 (1: -21.04 .. -48.66; 2: -5.62 .. -25.67 per APU); 3e13 with `DIST_CHUNKS=2` fits only with `BS_MDEV_LOGL` 28/29 and cap 0 (90.39 / +2.97 with `MN_T_CHUNK_MB=1024`, 88.24 / +5.12 with 512).
- `MN_T_CHUNK_MB` (512 / 256 vs 1024) only matters where `DIST_CHUNKS` is coarse (1, 2): -8.6 GB device at `DIST_CHUNKS=1`; none at 8 / 16.
- 3e13 with `DM_MN_WRAP` automatic turns the wrap on (device within 368.44) and so costs 7.64 GB per APU against `DM_MN_WRAP=0`; the auto rule compares the node-mean device figure, not the busiest APU. At 3.82e13 the wrap is off in every cell.

**Recommendation (mod).** For 3.82e13: `BS_MDEV_LOGL=28`, `MN_NONPOW2_CAP=0`, `DIST_CHUNKS=16` gives 85.98 GB per APU (+7.38 to the 93.36 edge; node-mean device 343.92 of 373.44; node 380.01 of 487), the only 3.82e13 setting above the 6.5 GB bar; today's S6 line (`BS_MDEV_LOGL=29`, default `DIST_CHUNKS`) has 89.74 (+3.62), below it. `BS_MDEV_LOGL=28` with the default `DIST_CHUNKS` has +5.77 (also below the bar). For 3e13: `BS_MDEV_LOGL=29` (or 28), cap 0, default `DIST_CHUNKS` is +7.92 (above the bar); `DM_MN_WRAP=0` with `DIST_CHUNKS=16` +18.12. Unproven: `DIST_CHUNKS=16` on path 1 layout D has not been timed or digit-checked (README: digits identical for 4 -> 8; the exchange messages halve against K=8's 64 KiB) and `BS_MDEV_LOGL=28` at scale has not run; both need the 1-node gate with `BS_LAYOUT_ONLY` / a 1e11 ABBA, and the 2-node gate, before they join ROUTE S6. Not adopted by this item (planner only).

**Measured, 1 node, aac7 (2026-10-28, item c05, cand3 c3-plan 73ef307 path 1 layout D, aac7 limits device 444 / node 420 via the profile, `ECALC_FIT_PER_APU=1 MEM_REPORT_DEVS=1`, `s4_ab.sh` ABBA with a discarded warm-up; NIC-free node).** A = `BS_MDEV_LOGL=29` (default `DIST_CHUNKS`); B = `BS_MDEV_LOGL=28 DIST_CHUNKS=16`. Digits identical to the known tails in all 9 + 9 + 5 + 5 runs (6.4e10 ...97846921235350161654; 1e11 ...37703810896884928977). Means over 4 runs per arm (2 rounds), seconds:

| size | arm | total | init | bs | dm | B - A (95 % interval, rounds) |
|---|---|---|---|---|---|---|
| 6.4e10 | A / B | 78.10 / 77.24 | 24.54 / 23.31 | 34.24 / 34.60 | 19.30 / 19.32 | -0.86 [-4.05, +2.34] (2), not resolved |
| 1e11 | A / B | 136.47 / 149.84 | 30.28 / 30.14 | 63.26 / 62.27 | 42.90 / 57.39 | **+13.37 [-0.46, +27.20] (2)**, +9.8 %, per-round +12.28 / +14.46, dm +14.5 s |
| 1e11, one round (2 runs per arm) | A / B1 = `DIST_CHUNKS=16` alone | 134.08 / 131.78 | 30.28 / 27.43 | 61.44 / 60.89 | 42.32 / 43.43 | -2.30 (1 round, no interval) |
| 1e11, one round | A / B2 = `BS_MDEV_LOGL=28` alone | 131.95 / 148.27 | 27.69 / 29.47 | 60.94 / 61.36 | 43.30 / 57.41 | **+16.32** (1 round), dm +14.1 s |

- The cost is `BS_MDEV_LOGL=28`, not `DIST_CHUNKS=16`: at 1e11 it moves the division from 42.9 to 57.4 s (about +14 s in each of the three rounds with `BS_MDEV_LOGL=28`), a +10 % total; at 6.4e10 the level split is not reached and nothing changes. `DIST_CHUNKS=16` is neutral at one node (no distributed exchange at size 1; -2.3 s is noise). The interval of the 2-round pair touches zero, but all three rounds with `BS_MDEV_LOGL=28` go the same way and the phase is the division.
- Per-APU peaks match the model and show no difference between the arms at 1 node: planner and measured driver maxima: 1e11 planner 91.85 GB per APU for A, B, B1 and B2 (the mean equals the max; unset `BS_MDEV_LOGL` gives 111.18 busiest / 96.68 mean, REFUSED by -0.18 against the aac7 111.0 per-APU limit); measured driver used 90.6 / 90.6 / 90.5 / 90.5 GB on APUs 0-3 in A, B, B1 and B2 (model +1.25 GB over the measurement); 6.4e10 planner 72.52, `mem` line 71.0 per APU. At one node both 28 and 29 already balance the four APUs at 1e11; the 576-node difference (29 leaves region 0 at 89.74, 28 balances at 87.59 at 3.82e13) cannot show at one node.
- Reading: the 576-node recommendation `BS_MDEV_LOGL=28` buys +2.15 GB per APU at 3.82e13 for about +14 s of division per 1e11 digits at one node (the share is 6.6e10 per node at 3.82e13, where the cost has not been measured; at 6.4e10 per node it was 0 here). `DIST_CHUNKS=16` (the larger part of the margin: +1.61 GB) has no one-node cost but its multi-node time and digits remain untested (2-node gate, 10-31). No ROUTE S6 change: adoption is the user's call.

**Where `BS_MDEV_LOGL=28` starts to cost on one node (measured, aac7 1 node, 2026-10-28, item c06, remote-v7 d6167b2, same method as above: A = `BS_MDEV_LOGL=29`, B = `BS_MDEV_LOGL=28 DIST_CHUNKS=16`).** Means per arm, seconds; digits identical within every size (6.632e10 ...79932777543051118449; 8e10 ...52640279071809056008; 9e10 ...32983517734368241080):

| digits | rounds | total A / B | division A / B | B - A (95 % interval) |
|---|---|---|---|---|
| 6.4e10 (above) | 2 | 78.10 / 77.24 | 19.30 / 19.32 | -0.86 [-4.05, +2.34] |
| **6.632e10 (the 3.82e13 per-node share)** | 2 | 76.36 / 75.82 | 18.09 / 18.05 | -0.55 [-1.76, +0.66] (-0.7 %) |
| 8e10 | 2 | 99.28 / 98.15 | 28.97 / 28.44 | -1.13 [-13.10, +10.84] (the wide interval is one round pair of 0.2 s and one of 2.1 s) |
| 9e10 | 1 | 106.29 / 106.89 | 29.25 / 29.66 | +0.60 (1 round, no interval) |
| 1e11 (above) | 2 | 136.47 / 149.84 | 42.90 / 57.39 | +13.37 [-0.46, +27.20] |

Reading: at the record's per-node share (6.632e10) `BS_MDEV_LOGL=28` costs nothing resolved (-0.55 s, interval +-1.2 s) and the same holds to 9e10; the division jumps from about 30 s at 9e10 to 57 s at 1e11, so the cost starts between 9e10 and 1e11 per node (the cliff, not a slope; not narrowed further). So at 6.632e10 the +2.15 GB per APU of `BS_MDEV_LOGL=28` is free on one node; its behaviour in the 576-node tree (different levels) is still untested. Decision on ROUTE S6 stays with the user.


**Mechanism and 576-node verdict for the `BS_MDEV_LOGL=28` cliff (2026-10-28, items c07 and c08, cand3 remote-v8 13322ef merged into c3-plan, aac7 NIC-free hold, 1 node).**

- (a) Planner diff, `MN_PLAN_ONLY`, aac7 limits, 28 vs 29 (modelled): at 9e10 and 1e11 the only change is leaf level 22, whose products (4.6e8 to 5.3e8 limbs, between 2^28 and 2^29) leave the batch tier and become device-tier dist_db products on 2^29-point planes (25.77 GB each). Leaf dist_db product counts rise by 20 at 1 node (for example 30 to 50, 34 to 54, 36 to 58 across the sizes tried). Tree top levels, reciprocal products, division products, grids and plane caps are identical. The plan therefore contains no division-side change; any division cost would come from device-tier leaf traffic interfering, not from a planned shape.
- (b) Placement (measured, 1 node, paired ABBA, digits identical in every run): 9.5e10, 1 round, B - A = +2.28 s total (dm 36.8 vs 38.2 s, not a division cost). 1e11, two new one-round pairs with the arm order swapped: +5.59 s (28 as B: 127.9 vs 122.3 s, bs +2.7 s, dm +2.8 s) and +3.87 s (28 as A: 126.6 vs 122.7 s, bs +1.6 s, dm +3.8 s), about +3 to +5 %. The earlier +13.4 / +16.3 s with division +14 s (c05) did not reproduce. Run-to-run spread within an arm is 1 to 3 s and aac7 has a documented slow mode (about +4 s on one run in three), so the earlier cliff is most likely a slow-mode artefact plus a small real cost; it is not located between 9.5e10 and 1e11 and not explained by a plan change. Doubt: 2 pairs at 1e11, no interval.
- (c) 576 nodes (planner, target limits, modelled): the same shape appears. At 3.82e13 level 22 gets 12 more dist_db leaf products (14 to 26) and at 3e13 8 more (10 to 18); the top tree levels and division products are unchanged by the switch. So the switch can reach the 1-node shape at 576 n, but it adds only leaf-tier device traffic, the same thing measured at 1 node as +3 to +5 % at 1e11 and no resolved cost at 6.632e10. Verdict: expect at most a few percent, no division cliff by mechanism. Not measured at scale; a 64-node run at 6.632e10 per node (A 29 vs B 28) is the cheap check and has not been run. The ROUTE S6 choice stays with the user.
