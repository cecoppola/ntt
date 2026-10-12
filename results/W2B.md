# W2B — wave 2, lane B of the settings removal (2026-10-12)

User-approved removal of experiment / adopted-default switches, lane B (transport, NTT kernels, distributed
products), batches B1, B3, B2, the optional B4 (TCP transport) and D1e (TASKS 27: six launch-line settings as
built-in defaults). Every removal keeps today's value, so digits are identical by construction; each batch was
gated on aac7 through the queue (SHMEM build, `-DCOMM_SHMEM` checked). Labels: m measured, mod modelled, a assumed.
Times Eastern.

## Batches

| batch | commit on main | switches | lines (git diff --stat, m) | gates (aac7, m) |
|---|---|---|---|---|
| B1 transport | 2646a63 | 12 + the comm_shmem.c `COMM_HS_TS` stamps | 11 files, +45 −322 | gate 2 n + 1 n PASS (1e9 identical, VERIFY OK); 2 n × 6.441e10/node VERIFY OK, 170.1 s |
| B3 NTT kernels | 3cfab5d | 12 + `ntt_b1_shoup` | 12 files, +156 −1039 | gate 2 n + 1 n PASS; `t_ntt hash` main vs B3: 2560 cases, 0 differing; `t_ntt 24`, `t_mul 20` VERIFY OK; ABBA 1 n 6.441e10, 4 pairs: B3 − main −0.40 s, 95 % CI [−3.84, +3.04] (null) |
| B2 distributed products | (this merge) | 16 | 17 files, +115 −473 | gate 2 n + 1 n PASS (twice: first base and after the A1/A3 rebase); 2 n × 6.441e10/node VERIFY OK, 167.3 s; `t_strategy 24 2 V` VERIFY OK; `mnaccept --only grid,mn` PASS; `MN_PLAN_ONLY=3.71e13:576` identical but for the P24 print |
| B4 TCP transport | (this merge) | 2 + value `tcp` | 23 files, +57 −776 | gate 2 n + 1 n PASS; 2 n × 6.441e10/node VERIFY OK, 158.0 s; `t_comm` 2 PEs (mnrun default shmem) VERIFY OK; `mnaccept --only grid` PASS |
| D1e TASKS 27 | (this merge) | 0 removed, 6 now default 1 | 16 files, +60 −53 | full stack: gate 2 n + 1 n PASS; gate 2 n with `COMM_LAYER_VSLOT_POOL=0 RNS_DIST_CACHE_PARTIAL=0 MN_OUT_DKM_HI=0` PASS; 2 n × 6.441e10/node VERIFY OK, 134.0 s; plans and BS_LAYOUT_ONLY identical, `mem_model --check-c` as on main |

Registry (`ecalc/switches.tsv`): 269 rows before wave 2 lane B → 204 on main with lane A's A1, A3, A4 (lane B removed 42 rows: B1 12, B3 12, B2 16, B4 2) after (m). README rows deleted row by row;
`tools/switch_registry.py --check` = 0 problems and `tools/profile_load.py --validate-all` = 0 problems on every branch.

### B1 — transport experiments (12 switches)
Removed: `COMM_SHMEM_RPIPE` (two-deep rounds, W_ROFF1/W_SIG1 words), `COMM_SHMEM_PEER_ORDER` (rotated ready-first
service), `COMM_SHMEM_KEEP_STAGING`, `COMM_SHMEM_THREAD`, `COMM_SHMEM_NOSYM`, `COMM_SHMEM_PUT_MB`,
`COMM_SHMEM_STAGE_SLOTS` (regions always on when sized), `COMM_LAYER_TEST_SKEW_MB`, `COMM_PUSH64` / `COMM_PUSH_BLOCKS`
(64-bit stores, 76 blocks hard-wired), `COMM_OFI_DUMP` (dump always), `COMM_OFI_SYM_TOP` (follows
`COMM_LAYER_VSLOT_POOL`; `tests/t_ofi_alloc` sets the mode directly), and the `COMM_HS_TS` stamps in `comm_shmem.c`
(the switch itself is lane A's A3). `t_ofi_alloc 1` fails exactly as on main (its pre-fix scenario; m).

### B3 — old NTT kernels (12 switches + the dead `ntt_b1_shoup` flag)
Removed at their defaults: `NTT_B16_BODY` (tile / radix-4 bodies of 7-stage passes), `NTT_MODMUL` (FP64 Barrett and
Shoup forms: `k_b1s`, the Shoup tables, `shoup_q`), `NTT_B1R` (the paper b1 pass `k_b1`, the 16-point form),
`NTT_PLAN` (plan_auto always), `NTT_R3_FUSE` (the fused radix-3 forms, `ntt_fwd_r3` / `ntt_inv_r3`), `NTT_B16_STG`
and `PW_FUSE` (env reads; `ntt_stg` / `ntt_pw_fuse` stay internal for the tests), `RNS_R3_MINK`, `RNS_BATCH_PAIR`,
`RNS_STRIPED_PAIR`, `RNS_PLANES_FIRST`, `RNS_POOL1_4Q`. `t_ntt`: the variant identity sections and variant benches
removed, `hash` kept for old/new diffs; `t_mul` 4b runs the paired form; `tests/n3x_node.sh` removed.

### B2 — distributed products (16 switches)
Deleted with their paths: `MN_REDIST_FUSE`, `RNS_CRT_SUB` (`k_crt_batch_m`, `k_crt_sub_merge`), `DIST_MN_SYM_SLABS`,
`RNS_STRATEGY_FORM` and the B4 form (`k_bsplit`, `k_b4_pw`, `b_tables`; `RNS_STRATEGY=B4` refused),
`DIST_TEST_LAG_MS`. Hard-wired: `RNS_DIST_CACHE` 0, `RNS_DIST_CACHE_FIT` 1 (the sized, budget-bounded slots are
unconditional; the key left the aac7/target lines, profiles, `target_kit.sh` — its `cache_guard` is no longer
needed — and `nodescreen.sh`), `DIST_PW_FUSE` 1, `DIST_TWREC_G` 1 (the plain packs stay as `gen_pack_bench`'s
reference), `DIST_LOGR_DELTA` 0, `DIST_SYM_SLABS` 1, `RNS_AUTO_PIECE_COST` 1, `RNS_STRATEGY_GRID` 1, `DIST_RB_WAIT` 1,
`COMM_ALLTOALLV_DEPTH` 2 (the one-deep v-exchange removed), `MN_P24` 2 (the 18-digit mn path stays as the fallback
when pool 0 has fewer than four planes). Fixed: the `plan cache` line printed `MN_P24=0` when unset; it prints 2.
`MN_PLAN_ONLY=3.71e13:576` on the aac7 line: identical to main except the config hash and that print (m).

### B4 — TCP transport (optional, user-approved; 2 switches + the value `tcp`)
`comm_tcp.c`, `COMM_HOSTS`, `COMM_PORT` and `COMM_TRANSPORT=tcp` removed. `mn.c` refuses `COMM_SIZE > 1` without
`COMM_TRANSPORT=shmem` (its TCP mesh paths removed); `mnrun.sh` sets `COMM_TRANSPORT=shmem` for more than one process
and no longer exports COMM_HOSTS/COMM_PORT; `tools/gate.sh` drops `COMM_PORT`. A build without SHMEM runs one
process only (aac6 multi-process runs need OSHMEM). `t_comm` and `t_dist`'s per-rank mode are SHMEM-only;
`wp6run.sh` and `tests/lay_host` (the host-only TCP harness of S45) removed.

### D1e — TASKS 27: six launch-line settings as built-in defaults
`COMM_LAYER_INTER2`, `COMM_LAYER_VSLOT_POOL`, `COMM_LAYER_VSLOT_SHARE`, `COMM_LAYER_VSLOT_RELEASE`,
`RNS_DIST_CACHE_PARTIAL`, `MN_OUT_DKM_HI` default to 1, each with its `=0` fallback; INTER2 and RELEASE follow
`COMM_LAYER_VSLOT_POOL` when unset (so `COMM_LAYER_VSLOT_POOL=0` alone restores the old form). The keys left the
aac7 and target lines and both profiles. `MN_PLAN_ONLY` with the new line (no keys) against main with the old line
at 3.71e13:576, 6.441e10:2 and 1.2882e11:2: identical except the B2 print fix (m).

## Cross-lane hunks (named for lane A)
- B2: `binsplit.c` (the `DIST_MN_SYM_SLABS` pool term, the v-slot depth), `mn_plan.c` (the P24 print).
- B4: `mn.c` (the TCP mesh paths in `mn_init`, `sub_mesh`, `sub_mesh_strided`, `mn_group_span`; `hosts_of`).
- D1e: `binsplit.c` (four one-line readers), `mn.c` and `ecalc.c` (one line each, plus `dkm_hi_explicit`).
- `rns_mul.c` hunks of B2 (CRT sub-stripes) and B3 (pairing / pool switches).
- `mem_model.py` (D1e): the X2 / S36 / M3 readers default to 1 as in the code, so `--check-c` stays as on main.
- Central docs: ALGORITHM.md (P24, B4, cache FIT, comm_tcp rows), ecalc/README.md; `tools/doccheck.py` 0 problems.

## Open items
- B2–D1e were gated on their own stack; after the final rebase onto main (lane A's A4, the doc rewrites R1–R4) they were
  build-checked on aac7 (SHMEM build) and doccheck / registry / profile checks rerun before the push (no new gate).
- The single 2-node 6.441e10 totals above are single runs (m), not timing claims; only B3 had a paired timing (null).
- aac6 multi-process runs now need OSHMEM (`COMM_TRANSPORT=shmem`); mnaccept's non-Cray path still omits it (mnrun now sets it).
- Old experiment drivers under ecalc/tests (pc_j*, ofi17*, std17*) still set removed keys; harmless (ignored).

## RESUME
Done: B1, B3, B2, B4, D1e merged to main. Worktrees removed (local and aac7); gate outputs on aac7 in ~/lbb1, ~/lbb3,
~/lbb2, ~/lbb2r, ~/lbb4, ~/lbd1e. Nothing running. Next lane-B work: none in this wave.
