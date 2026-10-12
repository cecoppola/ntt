# W2A: wave 2, lane A of the user-approved settings removal (2026-10-12)

Agent LA. Batches from the switch audit of 2026-10-12 (lane A): A1, A3, A4, A2, D5, A5, then TASKS 23. Every removed path
was off at the defaults and launch lines, or the default is hard-wired, so the digits are identical by construction; each
batch still ran the gates. Times Eastern. Lines are `git diff --stat` of the batch's commits (measured).

Gate per batch (aac7 queue item, 2 nodes, SHMEM build checked for `-DCOMM_SHMEM`): `tools/gate.sh --build --nodes 1` and
`--nodes 2` (10^9, aac7 launch line: VERIFY OK, digits identical to the reference, no segfault), then `ecalc/mnaccept.sh`
steps on 1 node as listed. No timing runs: no batch changes a kernel or a schedule at the defaults (hard-wired values only).

| batch | switches removed | lines (+/-) | gate | merged |
|---|---|---|---|---|
| A1 | `ECALC_VMM_BG=2` (value; `hipmm.c` and the link wraps), `ECALC_VMM_SAFE`, `ECALC_VMM_REPRO`, `ECALC_VMM_REPRO_SPIN`, `DBIG_ADDSUB2`, `DBIG_ADDSUB3`, `DBIG_QSEL`, `DBIG_MAXIDX_TOP`, `DBIG_MODQ_MONT`, `DBIG_S51_STATS`, `DBIG_BOUNCE_TS`, `DB_POOL_TRACE`, `MEM_COPY_WAIT_POLL`, `MEM_COPY_WAIT_REPORT_S` (13 + 1 value); `tests/t_s51_probe.c`, the S35 drivers | +66 / -546 | PASS: gate 1 node (ecalc 13.07 s), 2 nodes (20.28 s); mnaccept unit PASS (incl. t_dbig) | main a77a2f1 |
| A3 | `NEWTON_ANCHOR`, `NEWTON_LOWPROD`, `NEWTON_HIGHPROD`, `NEWTON_DKM` (on; `newton_dkm_set` stays for tests), `DM_MN_LEAN` (on; key dropped from the launch lines and profiles), `MN_MODEL_TCP`, `COMM_HS_TS` (7); `tools/g1_hsts.py` | +49 / -162 | PASS: gate 1 node, 2 nodes (18.56 s); mnaccept unit + corr PASS (sizes 1, 2, 4) | main 53bdfc1, 23224a9 |
| A4 | `ECALC_SPILL_DIR`, `SPILL_CHUNK_MB`, `SPILL_BUFFERED` (3): the spill primitive (no production caller), `tests/t_spill.c` | +6 / -385 | running | - |
| A2 | `MN_COMBINE`, `MN_DM`, `MN_LAYERED_RAW`, `MN_LAYERED_LOCAL`, `MN_SELFTEST_GROW`, `MN_TOPO_GROUP`, `BS_SCHOOL_NL`, `LIMB_BASE`, `MN_PLAN_DEXTRA`, `ECALC_SEED_ORDER`, `BS_POOL_RULE`, `BS_CKPT_TREE_BG`, `MN_TREE_EARLY_FREE`, `ECALC_VSLOT_BUDGET`, `COMM_LAYER_VSLOT_PREALLOC`, `COMM_SHMEM_POOL_AUTO`, `COMM_INIT_EARLY` (17); `ECALC_RES_LOG*` kept until TASKS 23 | +106 / -289 | `BS_LAYOUT_ONLY` byte-identical to the pre-A2 build at 6.441e10:576, 6.632e10:576, 6.441e10:16, 6.441e10:1, 4e10:4, 1e11:4 (plain and aac7 line); `mem_model.py --check-c` exact (0 terms off); queue gate pending | - |
| D5 | default `ECALC_OUT_PACKED` 1 -> 2 (60-bit packed, user decision; =0 ASCII and =1 stay); `t_patch` runs the three forms | +14 / -13 | queued (gate + mnaccept unit, e9, mn, recheck, corr) | - |
| A5 | `ECALC_OVERLAP`, `NEWTON_DEVICE` (optional, user-approved) | in progress | - | - |
| T23 | `DIST_LOGN_TEST` 4-node ">64 corrections" (results/A05.md) | not started | - | - |

Registry after each batch: `tools/switch_registry.py --check` 0 problems, `tools/profile_load.py --validate-all` 0 problems.
Switch count (registry rows): 257 at the start of lane A (main after B1) -> 244 (A1) -> 237 (A3) -> 234 (A4) -> 217 (A2).

## Notes

- A2: `out_stage`'s stand-in scatter branch (`mn_out_scatter_standin`, the size > 1 host X) is unreachable after MN_DM /
  MN_COMBINE but kept: the push leak guard rejects any diff whose lines or 3 context lines contain the C text `src` `.dev`
  (read as a domain), and that branch sits next to such lines. About 25 dead lines remain (ecalc.c 6, mn_out.c/h 19).
- A2: `comm_layered_create_minor` (lane B's file) has no caller after MN_TOPO_GROUP; left for lane B.
- D5: `mn_model.py PACKED_BPD` still prices the 64-bit form (8 B / 18 digits); the 60-bit form is 6.25 % fewer bytes.

## RESUME

State: A1 and A3 on main; A4, A2, D5 gates in the aac7 queue (items 44_la_a4, 46_la_a2, 47_la_d5; markers
`~/la_<batch>/DONE`); A5 being edited (branch la-a5, local). Branches are stacked: la-a4 -> la-a2 -> la-d5 -> la-a5;
rebase each onto main after the previous merges (`git rebase --onto <new base> <old base>`), regenerate the registry on
conflicts (`tools/switch_registry.py`, never hand-merge `switches.tsv` / `switches_gen.h`), build on the aac7 login node,
push main. T23 last: diagnose on a 4-node hold (1e9, `POOL_LOG=29 DIST_LOGN_TEST=23` vs 24, `ECALC_RES_LOG=1`
`NEWTON_VERBOSE=1`), then remove `ECALC_RES_LOG*`.
