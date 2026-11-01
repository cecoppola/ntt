# POOLRACE — free or remap of a dbig pool block while GPU work still uses it (C1POOL)

**Question.** At 3.7e13 digits and above six of six target runs failed (CRT flag = wrong transform-domain values; T1/VERIFY failures = wrong in-range limbs), the failure rate rising with size. One software hypothesis: `q_free` (`ecalc/dbig.c`) returns a block to the free extent list at once, with no stream synchronisation, and `vmm_make_room` unmaps "wholly free" VMM chunks and maps their handles again at the lowest run of empty slots (possibly the slots it just emptied) with no device synchronisation; a caller that frees while a kernel or transfer still uses the block, or a late write into a remapped chunk, would corrupt data silently. This report audits every free path by code reading and adds `DB_FREE_SYNC` to test the hypothesis on a machine.

**Change / branch.** Branch `c1pool`; switch `DB_FREE_SYNC` (default 0 = unchanged; `ecalc/README.md` row, registry, review date 2026-11-30). 1: `hipDeviceSynchronize` on the block's device before `q_free` puts the block on the free list and (in `q_alloc`, before the pool lock is taken, so before `vmm_make_room` can unmap any chunk) at every allocation; 2: the same on every device the process uses for the pool (peer readers of a quarter). A device on which the persistent seed-pull kernel (`k_pull`) is running gets `hipStreamSynchronize(0)` instead, since a device sync would wait for that kernel. No caller was found UNSAFE, so no caller was changed.

## 1. Audit (code reading, main 260d2c18; every line number is of `ecalc/` at that commit, not of the branch, where the switch code shifted them)

Facts the verdicts rest on (all read in the code):

- **dbig operations are synchronous.** Every kernel launch in `dbig.c` (`k_gather_shift`, `k_addsub3`, `k_carry`, `k_complement`, `k_maxidx`, `k_modq*`, `k_fill0`) runs on the null stream of its device and is followed by `hipStreamSynchronize(0)` (or a `hipMemcpy` that waits) in the same per-device thread before the operation returns (dbig.c 763, 946, 979, 999, 1069, 1139, 1152; `db_set_base_pow` uses `hipDeviceSynchronize`). Peer reads by kernels of another device finish inside the same operation. So a `db_free` after any dbig operation is ordered after all readers. `db_reserve` copies quarter-wise with `mem_dev_copy_on`, which ends in `hipStreamSynchronize(0)` (mem.c 171-183) before the old quarters go to `q_free` (dbig.c 578).
- **Remote puts do not land in pool blocks.** `comm_xshmem.c` stages every buffer that is not in the PE's symmetric DATA heap (`in_heap`, 165; `xex_post` 440-457) and copies received slabs out on the caller's stream followed by `SYNC` (`xex_wait` 508-510); the pool (VMM arena, hipMalloc regions) is not part of that heap. A peer's put therefore targets the heap, and the pool block is touched only by copies on the caller's own stream that are synchronised before `x_wait` returns. (comm_xshmem.c is outside this task and was only read.)
- **The seed streams (`g_h2d_s`, `g_pull_s`, `g_cs`) write only into the region arena before it is donated**; `db_pull_stop_all` waits for `done` and `exited` of every block of the pull kernels and `binsplit_seeds_*` wait before `donate_pools`. `g_bs` (host bounce) streams are synchronised inside `db_from_bi` / `db_to_bi` before they return.
- **hipMemUnmap ordering.** The HIP header documents no ordering against outstanding work (`hipMemUnmap`: "Unmap memory allocation of a given address range", beta); it is not stream-ordered. Treated as unordered: the caller must guarantee no work is in flight. UNCLEAR as to what the runtime does; irrelevant if all frees are ordered, as found below.

| # | free path (file:line) | who may still touch the block | synchronisation before the free | verdict |
|---|---|---|---|---|
| 1 | `db_free` -> `q_free` (dbig.c 556) after any dbig operation | peer-device kernels of the operation | the operation's own per-device `hipStreamSynchronize(0)` (above) | SAFE |
| 2 | `db_reserve` old quarters (dbig.c 578) | `mem_dev_copy_on` DMA reads | copy ends with `hipStreamSynchronize(0)` (mem.c 181) | SAFE |
| 3 | `db_pool_free` of `Y`, `pl[]` (rns_local.c 65, 94) | k_lload / NTT / CRT kernels on `v->s` | `hipStreamSynchronize(v->s)` at 63 and 93 | SAFE |
| 4 | `redistribute` `o->sb` (rns_dist.c 1097) | pack kernel on `s`; the exchange's staging copies | `HIP_SYNC(s)` at 1090; `comm_wait` syncs the stream (comm_xshmem.c 508-510) | SAFE |
| 5 | `oA.rb`, `oX.rb` (rns_dist.c 1476, 1486, 1492), `oB.rb` (1514) | gather kernels `k_gather_mn*` on `s` | `HIP_SYNC(s)` immediately before each (1476, 1486 explicit; 1491, 1512 per prime) | SAFE |
| 6 | `rbO` (rns_dist.c 1600) | `k_scatter_mn*` on `s` reading `rbO` and `dsI` | the spill exchange's `comm_wait` + `HIP_SYNC(s)` (1597) comes after the scatter launch and before the free | SAFE |
| 7 | `cx`, `dseg`, `tmp`, `gplan_free` (rns_dist.c 1608-1611, 1198) | CRT / transform kernels on `s` | `HIP_SYNC(s)` at 1546 and in every round; frees follow the parallel loop's last sync | SAFE |
| 8 | `spill_rb`, `sptab_d` (rns_dist.c 1381, 1626) | peer kernels of `db_share_add_spills_x*` | those are dbig operations (row 1) | SAFE |
| 9 | `sraw` (rns_dist.c 653) | `k_gather_raw`, `k_canon_pass` on `v->s` | `HIP_SYNC(v->s)` at 651 | SAFE |
| 10 | `sb`, `rb`, `dt` (rns_dist.c 2162, 2230) | pack / scatter kernels on `s`, exchange staging | `HIP_SYNC(s)` at 2161, 2228 and `comm_wait` | SAFE |
| 11 | partial transform-cache planes `cache_pc_give` (rns_dist.c 368) | D2D copies and NTT kernels on `s` of the product | per-prime `HIP_SYNC(s)` in the product loop; the give follows the product | SAFE |
| 12 | `shift_lane` `sb`, `rb`, `dp` (newton_db.c 481) | pack/scatter kernels on the non-blocking `g_ms` stream; exchange staging | `hipStreamSynchronize(st)` after each launch (473, 479) | SAFE |
| 13 | `mdb_to_host_all` `sb`, `rb` (newton_db.c 640) | `comm_allgather` fallback; `hipMemcpy` D2H | the following `hipMemcpy` is synchronous | SAFE |
| 14 | `db_free(&T)`, `Cm->sh` of rns_dist.c (855, 1621, 1632, 1767-1819, 2170, 2238) | dbig operations / products | rows 1, 5-10 | SAFE |
| 15 | newton_db.c `db_free` (33 sites), mn.c (8), ecalc.c 272-291 / 800 / 804 / 821, mn_out.c 988, binsplit.c 1714-1730 | dbig operations, products, the output writers' `dev_fetch` | `dev_fetch` syncs its stream per fetch (mn_out.c 105); ecalc.c keeps X until the early writer is joined (`mn_out_early_free`, 290) and holds `bs_Qd` until `bs_ckpt_bg_release` (821); the sites follow the operations of rows 1, 5-10 | SAFE (not every site read line by line: newton_db.c's 33 are dbig operations on dbig values) |
| 16 | region donation: `donate_one` -> `db_donate_adjacent` / `db_donate_ext` (binsplit.c 142-150, dbig.c 498-527) | the batch tier's kernels on the region pools; the seed streams | the batch products return synchronised; seed threads are joined; `db_vmm_arena_wait` first | SAFE (not proved for every level; no use found) |
| 17 | block-pool tail of a plane pool `db_donate_ext(..., 0)` (rns_mul.c 236) | plane kernels | after the pool's last product | SAFE |
| 18 | M3 scratch `db_pool_alloc` / `db_pool_free` users (rns_dist.c, newton_db.c) | as rows 4-13 | as above | SAFE |
| 19 | `vmm_make_room` unmap of wholly free chunks (dbig.c 425-453) | nobody, if every free was ordered (rows 1-18); the old VA of a chunk is not handed out again before the remap | none by itself (pool lock only); `hipMemUnmap` is not documented as ordered | UNCLEAR (SAFE given rows 1-18) |
| 20 | background mapper `vmm_bg_map` (dbig.c 320-349) | its own stream `st` zeroing the chunk it just mapped | `hipStreamSynchronize(st)` before `v->mapped` advances; `vmm_make_room`, `db_donate_adjacent` and `db_vmm_arena_wait` wait for `mapped` | SAFE |
| 21 | `db_vmm_arena_release`, `db_release_pools` (dbig.c 400, 545) | nobody (shutdown / phase end) | `hipFree` is implicitly synchronising; arena release follows `rns_shutdown` | SAFE |

Two non-ordering observations (not claimed to cause the failures):

1. `vmm_make_room` creates new chunks (`hipMemCreate`, growth) without zeroing them, while the background mapper zeroes each chunk and the hipMalloc fallback does `hipMemset`. A caller that relies on a fresh pool block being zero would see garbage after a growth; no such caller was looked for.
2. The pull kernels (`k_pull`) stay resident on a device until `db_pull_stop_all`; any code that calls `hipDeviceSynchronize` while they run would hang. `DB_FREE_SYNC` avoids that with the null-stream fallback (`g_pull_run`).

**Audit result: 21 rows, 0 UNSAFE, 1 UNCLEAR (row 19, safe if the others hold), the rest SAFE; rows 15, 16 are "SAFE by pattern, not every line read".** The code gives no support for the early-free hypothesis. It does not exclude a free by a path that is only reached at sizes the aac7 gates never run (>= 1e13 digits, 6+ node splits, `ECALC_PE_PER_GPU` layout) or a hardware / driver level race on `hipMemMap` / `hipMemUnmap` / `hipMemSetAccess` that no code reading can see. `DB_FREE_SYNC=1/2` adds a device sync before every free and allocation; if a driver-level race depends on timing, it may hide or move it, but it cannot prove its absence.

## 2. Method, results, verdict

**Question.** Does the dbig block pool free or remap memory while GPU work still uses it (`q_free`, `vmm_make_room`)?

**Change.** Switch `DB_FREE_SYNC` (default 0): 1 = device sync before each `q_free` and at the start of each `q_alloc` (outside the pool lock, which covers `vmm_make_room`); 2 = same, across all devices. Branch c1pool.

**Method.** Audit of all pool callers (table above); gates on aac7, 1e9 digits, 1 and 2 nodes; `mnaccept.sh --only unit,e9`; a search for a forced-pressure setting (VMM remaps visible with `DB_POOL_VERBOSE=1`).

**Results (measured, aac7, single 1e9 runs, not a timing result).**
- Audit: 21 rows, 0 UNSAFE, 1 UNCLEAR (the `vmm_make_room` unmap), the rest SAFE.
- 2 nodes: `DB_FREE_SYNC=0` PASS 14.62 s; 1 PASS 12.56 s; 2 PASS 12.48 s. 1 node: 0 PASS 8.00 s; 1 PASS 7.66 s. VERIFY OK, digits identical, no segv.
- `mnaccept.sh --only unit,e9` with `DB_FREE_SYNC=1`: rc 0; t_seed, t_dist, t_mn_grid VERIFY OK; e9 identical.
- `gate.sh --build` fails at "python unit tests" (`tests/test_site.py`), also on main: environmental, pre-existing. The build, registry check and doccheck were run by hand: rc 0.
- Pressure: 0 VMM remaps in 24 runs (4e9 and 1.6e10 digits; `ECALC_DEVICE_GB` 250/170, `DM_TIGHT=0`, `BS_ARENA_ROOM=0`, `BS_ARENA_TRIM_GB` 2..12; the trim is clamped to about 0.5 GB). All VERIFY OK. No pressure pair exists.
- ABBA cost of `DB_FREE_SYNC=1` (2 nodes, 1.6e10): NOT MEASURED. The driver failed on a missing `tools/rundriver.sh`. A replacement item (45_c1pool_abba2) is queued; whether it runs is the user's decision.

**Review fixes (after the first gates).** `g_pull_run` is now cleared in `db_pull_stop_all` once the kernels exited; the sync moved out of the pool lock; the init and time sum are thread-safe. The gate results above are of the code BEFORE these fixes; re-gate results (1 node, switch 0/1/2) are in the next line; the 2-node gates with the switch wait for a second NIC node and are queued as a separate item.

Re-gate after the fixes (measured, aac7, commit 7a4ab8fe, 1 node, 1e9 digits, single runs, not a timing result): `DB_FREE_SYNC=0` PASS (ecalc 9.18 s), `=1` PASS (7.86 s), `=2` PASS (7.96 s); VERIFY OK, digits identical, segv 0; registry and doccheck rc 0; checked: the item rebuilt (`make -B`) the binary from 7a4ab8fe just before the gates, and the three run logs show `DB_FREE_SYNC=0`, `1`, `2` in their config lines. 2-node re-gate (measured, aac7, item 47_c1pool_regate2, 2026-11-01 03:07-03:08 EST, same commit, 1e9 digits, single runs, not a timing result): `DB_FREE_SYNC=0` PASS (ecalc 15.13 s), `=1` PASS (11.74 s), `=2` PASS (12.15 s); VERIFY OK, digits identical, segv 0; build with the OFI flag, registry and doccheck rc 0.

**Remap path untested.** No remap occurred in any pressure run (0 in 24), so the `vmm_make_room` remap path was never exercised on hardware by this work.

**Verdict.** Inconclusive. No caller proven UNSAFE and no race reproduced; the switch is harmless in the gates (no slowdown seen at 1e9, one run each). The hypothesis is neither confirmed nor excluded at sizes where remaps occur. Recommendation: keep the switch (default 0), do not enable it.

**Open issues.** (1) Remaps were never exercised on aac7; a test that shrinks the arena directly is needed. (2) ABBA cost unmeasured. (3) UNCLEAR unmap row relies on the driver ordering of `hipMemUnmap`.

**RESUME.** Read the summary of queue item 45 (if done); fill the ABBA line and pressure pair here; otherwise design a forced-remap test.
