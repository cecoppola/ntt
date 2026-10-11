# archive/ manifest

Moved out of the working tree on 2026-10-10 (repo cleanup, audit AUDIT_REPO). Nothing was deleted except the two
byte-identical copies listed under s43. History is kept by `git mv`; use `git log --follow <new path>`.
Moved scripts still carry their original relative `source`/`$HD` paths and will not run from here without
restoring that layout (copy them back next to `ecalc/`, `tools/rundriver.sh`). They are records, not live tooling.

## drivers/ecalc/ (from `ecalc/`, 52 files)
Per-session batch drivers and their libraries; none is called by the Makefile, `target_kit.sh`, `mnrun.sh`,
`mnaccept.sh`, `accept.sh`, `e16_headline.sh`, `aac7env.sh`, `digcmp.sh` or `tools/rundriver.sh`.
| files | session / phase | note |
|---|---|---|
| `s20_batch` .. `s23_batch`, `s29_m2`, `s32_batch`, `s33_batch`, `s34_batch` | S20-S23, S29, S32-S34 | A/B batches, results/S20..S34 notes |
| `s27/s28/s30/s31_lib.sh`, `s27/s28/s30/s31_batch.sh`, `s27/s28/s30/s31_xagg.awk`, `pairstats.awk`, `s32_pairsoak`, `s34_pairsoak`, `s34_stats.awk` | S27-S34 | source each other; moved as one set |
| `c314_batch`, `c314_batch2`, `variance.py` | phase 14 | |
| `dkm15_batch`, `dl15_batch`, `ew15_batch`, `k15_batch`, `int315_job`, `int315_plans`, `mpb15_job`, `np15_job`, `p15_job`, `p24_job`, `wm15_job`, `ladder1n`, `a14_soak`, `s14_batch`, `t10_d5`, `t10_d5b`, `t10_test`, `v11_d5`, `v11_recheck`, `g13d_chain`, `g13d_hang`, `g13d_run`, `n13_ckpt_test`, `ackpt_test`, `tgtb2_est` | phases 10-15 | |
Kept live in `ecalc/`: `target_kit.sh mnrun.sh mnaccept.sh accept.sh e16_headline.sh aac7env.sh digcmp.sh`
and, because docs or code still cite them, `closing.sh`, `variance.sh`, `wp6run.sh`.

## drivers/tools/ (from `tools/`, 7 files)
`s38_abba.sh s40_abba.sh s42_abba2.sh s42_hang.sh s44_abba.sh s44_node.sh s44_reg.sh` (S38, S40, S42, S44 A/B and hang
drivers; `s44_reg` calls `s44_node`, `s40_abba` calls `s38_abba`). `tools/rundriver.sh` and the rest stay.

## drivers/s43/ (from top-level `s43/`)
`s43_batch.sh s43_job.sbatch s43_stats.py`. Removed as byte-identical duplicates: `s43/rundriver.sh` (= `tools/rundriver.sh`)
and `s43/s31_lib.sh` (= `ecalc/s31_lib.sh`, now `drivers/ecalc/s31_lib.sh`); `s43_batch.sh` sources both from its own
directory, so to rerun copy them back next to it.

## drivers/kp15_batch.sh (from `results/`)

## Superseded design documents (from the top level, now in `archive/docs/`)
`CODE_REDUCTION.md DECISIONS.md DECISIONS2.md DECISIONS3.md DESIGN.md ALGORITHM.md`: banner HISTORICAL, superseded by
internal notes. References in PLAN, TASKS, RESULTS and results notes now
carry the `archive/docs/` prefix.

## results-raw/ (from `results/`)
Raw logs/data (tracked) of: `INT315 NP15 MPB15 CX15 T215 AS15 MS15 SC15 DL15 DOC215 N3x15 S115`; the report
`results/<X>.md` of each stays in `results/` and now points here. `loose/`: `D13b_calibrate.txt`, `D2_13d_plan1_C.txt`,
`D2_13d_steps1.txt`, `S13_e2.txt`, `e40b-report.html`.
Kept in `results/` because code or live docs read them: `L815` (tests/l8_*.py), `PC15 P2415 V314 P15`, the
`D13b_strategy_e0 D2_13d_plan576 L13d_plan576 S13_e0 mrun_13b` text files and the other small directories.

## Documentation consolidation, 2026-10-10 (now in `archive/docs/`)
`TASKS_HISTORY.md`: the old front of `TASKS.md` (Phase 12-13 log, sections 1-7); `ECALC_README_PREAMBLE_2026-10-05.md`: the
history of the defaults and of the target size that headed `ecalc/README.md`.

## RESULTS_ARCHIVE_1-114.md (from `RESULTS.md`, 2026-10-10)
Sections 1-114 of `RESULTS.md` (campaigns 1-4, Phases 1-17 through 2026-10-06), moved verbatim. `RESULTS.md` keeps an index of all
sections and the full text of section 115 onward.

## Left in place on purpose
`diff run suite isa.py envcheck.sh nodecheck.sh` (top level): the microbenchmark suite is live (Makefile `make isa`,
`ecalc/README.md` uses `./run`, `suite` calls `envcheck.sh`, `bench/README.md` cites them). `PLAN.md` (now bannered as wholly historical),
`TASKS.md` (now the live list) is not moved; `RESULTS.md` is split (see above) (user instruction).

## Archived experiment branches (tags, 2026-10-10)

Annotated tags `archive/<branch>` on GitHub cecoppola/ntt (branches deleted locally and on GitHub; recover with
`git checkout -b <branch> archive/<branch>`): archive/p14-L1, archive/p14-R1, archive/p15-M6, archive/p15-MAP,
archive/p15-RL, archive/p15-SX, archive/rl-fill, archive/s24 (NTT3P bench, shelved), archive/s25 (10-node drivers),
archive/s37. Branches fully merged into main (29, b7-vslot ... s45, tgtbench2) were deleted without tags.
Kept as branches: `s35` (crash fix, deferred), `claude/confident-mayer-yx23pt`.

Per-session drivers of the 2026-10-11 batch: `archive/drivers/s46/` (s46_drive, s46_soak6, s46_arm6, s46_stats), `s47/`, `s48/`, `s49/` (s49_*), `s50/`, `s51/`.

## Code reduction 2026-10-11 (results/A11.md tiers A and B; agent b02, branch b02)

Deleted from the working tree, not moved. The parent commit is tagged `archive/pre-reduction-2026-10-11` (bc80001) on
GitHub; recover any item with `git show archive/pre-reduction-2026-10-11:<path>` or `git diff <b02 commit>^ <b02 commit>`.
| item | removed | switches | evidence |
|---|---|---|---|
| A1 | engine 2 (two 62-bit primes / 45-bit points): `ecalc/ntt2.c ntt2.h modarith2.h crt2.c tests/t_ntt2.c`; `rns_mul.c` `rns2_core`, `rns2_mul_mdev`, `rns2_mul_batch` and their dispatch; `crt.h` `crt2_carry`; `dbig.c` VMM guard; `ecalc.c` META `engine=`; Makefile and `tests/cx_build.sh` objects. Closes bug C1 (`rns2_mul_batch` ignored its added operand) by removal | `RNS_ENGINE` | RESULTS §44 (archive/RESULTS_ARCHIVE_1-114.md): 641 s vs 291 s at 4×10¹⁰, peak 282 vs 248 GB (measured) — engine 1 adopted |
| A2 | rejected placement / layout / allocation experiments: `bigint.c` THP form of `bi_alloc_huge`; `mem.c` `mem_pin_threads` and the `MEM_ALLOC` forms (`alloc_form`, fine/uncached/managed/host/mmap) of `mem_dev_malloc`; `rns_mul.c` quartered CRT layout (+ `crt.c` `crt_carry_par4_q`) and the wide repack; `ecalc.c` `pq_bg` pool pregrow and the stop-after-bs exit; `binsplit.c` flat region sizing, region slack and the `ECALC_DM_POOL_K` arena floor; `comm_ofi.c` transmit-complete diagnostic; `comm_shmem.c` Phase-11 fence switch; `ntt.c` ds_swizzle B<->C exchange (`xchg16`, template parameter `SW` of `k_b16r`); tests `t_ntt` (xchg sweep, body 4 rate), `t_mul` (layout repeat), `t_crt` (quartered checks and bench) | `BI_HUGE`, `MEM_PIN`, `RNS_CRT_LAYOUT`, `RNS_REPACK_WIDE`, `ECALC_POOL_GROW_GB`, `BS_REGION_FLAT`, `BS_REGION_SLACK`, `COMM_OFI_DC`, `COMM_SHMEM_FENCE`, `NTT_B16_XCHG`, `MEM_ALLOC`, `ECALC_STOP_AFTER_BS`, `ECALC_DM_POOL_K` | RESULTS §27, 38, 49, 55, 56, 70 (archive/RESULTS_ARCHIVE_1-114.md), results/I.md, results/A-mem.md, results/XEFF (X3): each measured no gain or harmful |
| A3 | debug instruments with no script, test or launch use: `binsplit.c` `bs_dump` and the leaf/level raw dumps (+ `ecalc.c` leaf dump), the odd-node copy probe (`rns_mul.c` `rns_copy_probe_*`, `rns_t_launch`, `cp_poll`), the grpB operand snapshot (`rns_snap_check`, `g_snap*`); `mem.c` copy-no-wait and no-memset test hooks; `dbig.c` one-thread quarter driver (`g_par`) and the cross-device page warm (`k_touch`); `comm_shmem.c` `TRACE` macro and `g_trace`; `mn.c` topology-mesh trace and the layered self-test value search | `ECALC_LEAF_DUMP`, `ECALC_COPY_PROBE`, `ECALC_B_SNAPSHOT`, `MEM_COPY_NOWAIT`, `DBIG_SERIAL`, `DBIG_WARM`, `MEM_NO_DEV_MEMSET`, `COMM_SHMEM_TRACE`, `MN_TOPO_TRACE`, `MN_DEBUG` | Phase 12 R witnesses (results/R.md: race fixed, 56/56 identical); the rest are print-only debug hooks (PLAN 28.1 group (a), approved 2026-09) |
| B4 | legacy-flow guards of the decimal device flow: `ecalc.c` staging modes 0/2 (`binsplit_seed_stage_bytes`); `binsplit.c` host region pools (`bs_regions_on_device`), the host mdev tier (`bs_dev_mdev`, the mdev-level host-pool branch) and the no-tail arena layout (`tail_on`); `mn_plan.c` `devm`; `rns_mul.c` striped-only batch form (`rns_batch_local`) | `ECALC_STAGING`, `BS_DEVICE_POOLS`, `BS_DEV_MDEV`, `RNS_BATCH_LOCAL`, `ECALC_TAIL` | rollbacks of defaults adopted in Phases 8-13 (RESULTS §55, 64; Phase 11 M); `NEWTON_DEVICE=0` and `ECALC_OVERLAP=0` kept |
