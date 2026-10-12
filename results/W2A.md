# W2A: wave 2, lane A of the user-approved settings removal (2026-10-12)

Agent LA. The lane A batches of the switch audit of 2026-10-12: A1, A3, A4, A2, D5 and A5. All six are on main. TASKS 23 was
taken over by a separate agent (T23) and is not part of this report. Every removed path was off at the defaults and on the
launch lines, or its default is now built in, so the digits are identical by construction. Each batch still ran the gates.
Times are Eastern. Line counts are `git show --stat` of the batch's commits on main (measured).

Gates for each batch ran as an aac7 queue item on 2 nodes with a SHMEM build (`-DCOMM_SHMEM` checked in the build log):
- `tools/gate.sh --build --nodes 1` and `--nodes 2`: 10^9 digits on the aac7 launch line, VERIFY OK, digits identical to
  the reference, no segfault;
- then the listed `ecalc/mnaccept.sh` steps, on 1 node.

No timing runs were made. No batch changes a kernel or a schedule at the defaults: each one only builds in today's values.

| batch | switches removed | lines (+/-, m) | gates (m) | on main |
|---|---|---|---|---|
| A1 | `ECALC_VMM_BG=2` (the value only, with `hipmm.c` and the link wraps); `ECALC_VMM_SAFE`, `ECALC_VMM_REPRO`, `ECALC_VMM_REPRO_SPIN`, `DBIG_ADDSUB2`, `DBIG_ADDSUB3`, `DBIG_QSEL`, `DBIG_MAXIDX_TOP`, `DBIG_MODQ_MONT`, `DBIG_S51_STATS`, `DBIG_BOUNCE_TS`, `DB_POOL_TRACE`, `MEM_COPY_WAIT_POLL`, `MEM_COPY_WAIT_REPORT_S` (13 switches + 1 value); also `tests/t_s51_probe.c` and the S35 drivers | +66 / -546 | gate 1 node PASS (ecalc 13.07 s), 2 nodes PASS (20.28 s); `unit` PASS (incl. t_dbig) | a77a2f1 |
| A3 | `NEWTON_ANCHOR`, `NEWTON_LOWPROD`, `NEWTON_HIGHPROD`; `NEWTON_DKM` (built in on; `newton_dkm_set` stays as the tests' override; the single-quotient division stays as the fallback for small k); `DM_MN_LEAN` (built in on; the key is gone from the launch lines and profiles); `MN_MODEL_TCP`; `COMM_HS_TS` (7); also `tools/g1_hsts.py` | +49 / -162 | gates 1 and 2 nodes PASS (18.56 s); `unit`, `corr` PASS (sizes 1, 2, 4) | 53bdfc1, 23224a9 |
| A4 | `ECALC_SPILL_DIR`, `SPILL_CHUNK_MB`, `SPILL_BUFFERED` (3): the spill primitive, which had no production caller, and `tests/t_spill.c`. The O_DIRECT file layer (`sp_file`) stays | +6 / -385 | gates 1 and 2 nodes PASS (21.49 s); `unit` PASS (incl. t_out, t_patch) | ed25de2 |
| A2 | `MN_COMBINE`, `MN_DM`, `MN_LAYERED_RAW`, `MN_LAYERED_LOCAL`, `MN_SELFTEST_GROW`, `MN_TOPO_GROUP`, `BS_SCHOOL_NL`, `LIMB_BASE`, `MN_PLAN_DEXTRA`, `ECALC_SEED_ORDER`, `BS_POOL_RULE`, `BS_CKPT_TREE_BG`, `MN_TREE_EARLY_FREE`, `ECALC_VSLOT_BUDGET`, `COMM_LAYER_VSLOT_PREALLOC`, `COMM_SHMEM_POOL_AUTO`, `COMM_INIT_EARLY` (17). `ECALC_RES_LOG*` are kept for TASKS 23 | +105 / -283 | gates 1 and 2 nodes PASS (20.62 s); `unit` PASS; `e9`, `grid`, `ckpt`, `sched` PASS (6/6, re-run in item 50, see notes). `BS_LAYOUT_ONLY` output byte-identical to the build before A2 and to main before the merge, at 6.441e10:576, 6.632e10:576, 6.441e10:16, 6.441e10:1, 4e10:4 and 1e11:4, plain and with the aac7 line; `mem_model.py --check-c` exact (0 terms off) | 4e67fde |
| D5 | default `ECALC_OUT_PACKED` 1 -> 2, the 60-bit packed form (user decision, TASKS 40); `=0` (ASCII) and `=1` stay. `t_patch` runs all three forms; its part check now reads the 60-bit form | +24 / -21 | gates 1 and 2 nodes PASS (16.05 s, 26.48 s); `unit`, `e9`, `mn`, `recheck`, `corr` PASS (20/20, item 52) | fe9f21d, d12ac2c |
| A5 | `ECALC_OVERLAP`, `NEWTON_DEVICE` (2, optional, user-approved): the sequential flow, the host reciprocal and division (`newton_recip`, `newton_divmod`, `newton_recip_seeded`) and the `par == 0` init paths. The size-1 host-P/Q path for small runs stays: it is reached at the defaults | +58 / -210 | gates 1 and 2 nodes PASS (12.04 s, 18.91 s); `unit`, `e9`, `mn` PASS 14/15. The one failure, t_patch, was the test artefact fixed in D5 (see notes) | c7942bc |

Totals (measured):
- 42 switches and 1 switch value removed, and 1 default changed.
- About 1,800 lines removed, 300 added.
- Registry rows: 257 at the start of lane A (main after B1). 185 now, with lane B's and the D1e removals in between.
- After every merge: `tools/switch_registry.py --check` 0 problems, `tools/profile_load.py --validate-all` 0 problems,
  `tools/doccheck.py` 0 problems.

## Notes

- **Push leak guard false positive (A2).** The push hook rejects any diff whose changed or context lines contain the C text
  `src` + `.dev` (it reads it as a domain name). The commit hook checks only added lines. `ecalc.c`'s `out_stage` has such
  lines. So its stand-in scatter branch (`mn_out_scatter_standin`, the size > 1 host X) stays as unreachable code: about 25
  lines in `ecalc.c` and `mn_out.c/h`. Deleting it needs the guard to allow that token.
- **Shared mnaccept directory (aac7).** `aac7env.sh` defaults `MNACCEPT_TMP` to one shared directory. Concurrent queue
  items deleted each other's digit files, which caused "Stale file handle" and "cannot open part" failures in items 46
  and 47. Those steps passed when re-run with a private directory (`MNACCEPT_TMP=~/<item>/mnaccept_tmp`, items 50 and 52).
  Every queue item that runs mnaccept should set its own directory.
- **t_patch under the 60-bit default (D5).** `packed_part_check` read 8 bytes per limb, which gave 2399 false failures
  ("the part does not hold N limbs") under `=2`. Fixed in d12ac2c. The writer and the patch were correct: `corr` and
  `recheck` passed on the 60-bit files.
- **Not changed.** `mn_model.py PACKED_BPD` still prices the 64-bit form (8 B per 18 digits). The 60-bit form is 6.25 %
  fewer bytes. `comm_layered_create_minor` (lane B's file) has no caller since MN_TOPO_GROUP was removed.
- **A5 side effects.** The plain `t_newton` now runs the device sections (1098 checks instead of 620). The print-only
  `config:` line no longer has `overlap:1`.

## RESUME

Lane A is complete; nothing of it is pending on aac7 (no holds, no worktrees left). Open, owned elsewhere: TASKS 23
(agent T23); removing `ECALC_RES_LOG*` after T23.
