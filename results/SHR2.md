# SHR2 — shrink pass 2: top-level checkpoints only, the unreachable stand-in, SOS/OSHMEM builds, AUD2 re-scan (2026-10-12)

**Question.** Remove the code the user approved for removal on 2026-10-12, without changing a digit: D11 (keep only the
top-level checkpoint set: no restart inside a run), D16 (the unreachable stand-in scatter of the output stage), D14 (the
SOS / OSHMEM build branches, after mnrun.sh dropped their launch paths), and the re-scanned AUD2 items 1 (remainder), 4,
5, 6 and 8 (`results/AUD2.md`, `results/AUD3.md`); then the residue log `ECALC_RES_LOG*` (held for T23, which closed).

**Change / branch.** Branch `shr2`, rebased on main d1d19bf (after P1 merged); eleven commits. No switch added; no default
changed. `rns_dist.c` and `mdb.h` were not touched (on open branches during the work); `newton_db.c` only after P1 merged.

## What was removed (lines: `git show --numstat` of each commit, measured)

| item | what | + / − lines |
|---|---|---|
| D11 | the leaf-level (level-loop) snapshots and their restart, the tree sets below the top level and their removal protocol, the restart scan and schedule comparison (`mn_ckpt_tree_level`, `bs_ckpt_restart_scan`), the abort test hooks, bs_stats' checkpoint and restart counters. Switches: `BS_CKPT_EVERY`, `BS_CKPT_MIN_LEVEL`, `BS_RESTART`, `BS_CKPT_TREE`, `BS_CKPT_TREE_EVERY`, `BS_CKPT_ABORT`, `BS_CKPT_ABORT_TREE`, `BS_CKPT_ABORT_NODE` (8). The top-level set (`ECALC_CKPT_TOP`, its background writer, budget, header v3; v2 sets still read) stays; `BS_CKPT_DIR` now only names its directory and writes nothing alone. `mnaccept.sh`: the `ckpt` step tests the top set; the `sched` step is gone | +99 / −461 |
| D16 | `out_stage`'s stand-in scatter branch and `mn_out_scatter_standin` (size > 1 always has the sharded X) | +6 / −34 |
| D14 | Makefile `SHMEM_HOME` (SOS) and `oshcc` (OSHMEM) branches; `COMM_SHMEM_SOS` (external heap) and `COMM_SHMEM_OSHMEM` code in `comm_shmem.c`; README rows `COMM_SHMEM_SERIAL`, `COMM_SHMEM_DEVHEAP`, `COMM_SHMEM_ORDER`. The Cray path is unchanged | +34 / −109 |
| AUD2 6 + AUD3 | no caller anywhere: `bi_sub_shifted`, `bi_bits`, `binsplit_dm_hole_bytes`, `comm_shmem_size`, `db_pool_tail_bytes`, `db_pool_tail_stats`, `db_pool_vmm_stats`, `db_pool_largest_free`, `db_pregrow`, `db_share_add_spills`, `ec_add`, `ec_sub`, `ec_shoup`, `ec_shoup_pre`, `k_twiddle`, `rns_ensure_staging`, `rns_pool_n_grow`; `comm_layered_create_minor` with the layered communicator's intra-minor rank order; the unreachable host `rns_mul_pair` branch of the mdev levels (`binsplit.c`) | +14 / −151 |
| AUD2 4 | `dist_pack_bench` and the kernels only it launched (`k_pack`, `k_pack_tiled`, `k_twpack`, `k_unpacktw`, `k_unpack_rows`), `t_dist`'s `DIST_PBENCH` mode (rows `DIST_PBENCH`, `DIST_PSIZE`) | +6 / −136 |
| AUD2 1 | `tests/t_nicbw.c` and rows `NICBW_HHM`, `NICBW_SHHEAP`, `NICBW_SYS` | +2 / −268 |
| AUD2 5 (part) | `t_ntt bench` (K13 / K13b microbenchmarks) with `ntt_pass`, `ntt_pass_at`, `ntt_pass_bounds` | +0 / −231 |
| AUD2 8 (part) | one `digits_format` in `tests/harness.h` (t_out, t_patch) | +13 / −22 |
| RES_LOG | `ECALC_RES_LOG`, `ECALC_RES_LOG_LEVEL`, `ECALC_RES_LOG_CPU` with every check they switched on (`bs_res_check`, the leaf hand-over check, `tree_check`, the output stage's RES lines, `db_mod_qs`'s host cross-check, `mdb_mod_qs`'s RES print) | +6 / −121 |
| (comments) | the history narration SHR1 had to leave (comm_ofi.c, ecalc.c) | +4 / −4 |
| **total** | 39 files (`git diff --shortstat` against main d1d19bf) | **+178 / −1531** |

Settings (registry rows, `tools/switch_registry.py`, measured): **186 before** (185 at the start plus P1's `DM_MN_WRAP`),
**170 after**: 16 removed (8 checkpoint, 3 residue log, 3 `NICBW_*`, `DIST_PBENCH`, `DIST_PSIZE`).

Archived (local archive, MANIFEST line): `removed_2026-10-12/ecalc/tests/t_nicbw.c` (byte-identical) and, under
`removed_2026-10-12/shr2/ecalc/`, the pre-change `ntt.c`, `tests/t_ntt.c`, `ntt_dist.c`, `tests/t_dist.c`.

## Method

Build on the aac7 login node (worktree from the cluster clone): `make -B SHMEM_CRAY=1 GMP_HOME=~/gmp ecalc` then `make all`
(tests): rc 0, 0 warnings, `-DCOMM_SHMEM` in the log. Gates as one aac7 queue item (`81_shr2_gate.sh`, 2 nodes):

    tools/gate.sh --build --nodes 2 --hold <hold>      # aac7 line; P24 forced at 2 nodes (--p24 auto)
    tools/gate.sh --nodes 1 --hold <hold>
    MNACCEPT_TMP=~/shr2/mnaccept_tmp ecalc/mnaccept.sh <hold> --only unit,e9,ckpt

## Results

All measured on aac7, queue items 81 (commit e181111, before the residue-log removal), 82 (90ad09c, after it, on main f08a4ea)
and 83 (0eeb624, the final tree on main d1d19bf); every build rc 0 with 0 warnings and `-DCOMM_SHMEM`.

| check | 81 | 82 | 83 (final) |
|---|---|---|---|
| gate 2 nodes, 10⁹, aac7 line, P24 forced | PASS, identical, 20.40 s | PASS, identical, 17.05 s | PASS, identical, 23.91 s |
| gate 1 node, 10⁹ | PASS, identical, 11.72 s | PASS, identical, 11.86 s | PASS, identical, 12.70 s |
| unit `t_ntt 24`, `t_mul 20`, `t_newton 20`, `t_verify`, `t_out`, `t_patch` | PASS | – | – |
| unit `t_dbig 0`, `t_bs` | PASS | PASS | PASS |
| unit `t_mn_grid` at 2 processes | cut by the item's own 1500 s cap while passing its shapes | PASS (180 checks) | PASS (180 checks) |
| `e9` size 1 | not reached | PASS, identical | PASS, identical |
| `ckpt`: top set written (2 headers, nothing else) and read by the recheck | not reached | PASS (3 RECHECK OK) | PASS (3 RECHECK OK) |
| `ckpt`: budgeted drop (`ECALC_CKPT_TOP=2`, 1 MB/s emulated) | not reached | PASS (2 ABANDONED, no header) | PASS |
| `ckpt`: `BS_CKPT_DIR` alone writes nothing | not reached | PASS | PASS |
| `recheck` e9 size 1, e8 size 2 (default `<outfile>.top`; corrupted copy fails) | – | – | PASS, PASS |

The ecalc totals are single runs on a shared machine, not timing claims. The removed code was off at the defaults and on
the launch lines (or, D11, reached only with switches that are now gone), so no timing was measured.

## Verdict

Merged: every gate and mnaccept step PASS with digits identical; 1531 lines and 16 settings fewer.

## Open issues (left for a later pass)

- **Host product tier** (`rns_mul`, `rns_mul_low`, `rns_mul_mdev`, `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`,
  `mul_rec`, `mdev_gpu_crt`; ≈ 270 lines of `rns_mul.c`): no production caller any more (its last one, the unreachable mdev
  branch, is gone), but `t_mul`, `t_dbig`, `t_mn_grid`, `t_newton`, `t_primes` use `rns_mul` as their reference product.
  Removing it needs a test-side reference that is fast above 2^20 limbs (the GMP string bridge of `harness.h` is not).
- Skipped because `rns_dist.c` was on an open branch (p1, now simp): `gen_pack_bench` and its kernels (`DIST_GBENCH`),
  `rns_mul_dist_hd`, `acc_flat`, `part_owner`, `rns_mul_dist` (test-only), the duplicate `pow_table` / `dev_pow_table`.
  Not done for time: the duplicate `mn_powmod` (newton_db.c) / `powmod_h` (dbig.c); `newton_dkm_set` stays (the tests'
  override of the DKM choice).
- The open branches simp, n3 and k4b (opened during this work) edit `binsplit.c`, `ecalc.c`, `rns_mul.c`, `rns_mul.h`;
  they need a rebase over this merge.
- Kept on purpose (test oracles or checks of production code): `ntt_host_fwd/inv`, `crt_garner3/4`, `db_pool_extents`,
  `db_mod_q`, `db_set_base_pow`, `vf_digits_mod`, `tier1/tier2`, `dist_inv_t`, `ec_shoup_lazy`.
- Duplicates not merged: the device Garner of `rns_mul.c` / `p24_crt.c` (`dmod128`, `dmod192`) and `k_store` /
  `k_copy_h2d` (kernels in two translation units: a shared header would change code generation, so it needs its own check);
  `make_mdb` of `t_mn_grid.c` / `cx_grid.c` (cx_grid has no Makefile rule).
- `t_ntt bench` is cited by `results/K13.md`, `K13b.md`, `NTT3P.md` (history; the code is in the archive).

## RESUME

Done: merged to main and pushed; worktrees removed (local and aac7); no job of ours left (queue items 81-83 finished, their
holds cancelled by the runner). Gate logs on aac7 under `~/shr2/Q*/` (raw, not in git). Next (a later pass): the host
product tier with a test-side reference product; the rns_dist.c items above once simp has merged.
