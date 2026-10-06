# STD17 — OFI memory accounting (OFIMEM) and the standard 10-node run with all NICs (Phase 17, branch `p17-ofimem`)

Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured** / **modelled** / **assumed**.

## RESUME (2026-10-06 11:45 EDT)

- **Part A (code): done, merged to `main`.** Commits on `p17-ofimem`: `9e42e7a` (accounting), `0c77c4c` (driver), + docs/this file.
- **Part B: armed, not finished.** Driver `~/ofimem17/ecalc/tests/std17_drive.sh` (aac7 clone `~/ofimem17` at `0c77c4c`, built
  `make -s -j48 SHMEM_CRAY=1 GMP_HOME=~/gmp` + `make -C ../tools` after `source ecalc/aac7env.sh`), `setsid nohup`, **PID 2373608** on
  the aac7 login node uan1, armed 11:20 EDT. It waits for `~/ofi17/HANDOFF_DONE` (deadline 20 h), then for job 12287's other steps
  to end (≤ 2 h), reads MemAvailable on the 10 nodes and picks D per node from the OFI layout table (81e9 → 420.66 GB, 78e9 → 412.07,
  74e9 → 403.48, 70e9 → 394.89, modelled) with node ≤ min MemAvailable − 12 GB, runs the plan + layout, a 2-node sanity run (2e10),
  the record (timeout 9000 s), RECHECK (packed, 9000 s), the 1e11 prefix (`~/ref/e_1e11.out`, else `~/ntt/ecalc/results/e_1e11.out`),
  deletes the parts, and **always** writes `~/ofimem17/R/STD17_DONE` (first line = verdict, then walls.txt).
- **Collect:**
  ```
  R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com"
  $R 'cat ~/ofimem17/R/STD17_DONE; tail -40 ~/ofimem17/R/log/std17.log | cut -c1-300'
  $R 'pgrep -af "^bash tests/std17_drive.sh"; squeue -s -j 12287 -h -o "%i %j %M"'          # kill only by that PID
  $R 'L=~/ofimem17/R/log/n10_record.log; grep -E "^total|RESULT ecalc|comm-mark|VERIFY OK" $L | cut -c1-260; grep -E "comm_ofi: device .*pool peak|comm_shmem: pe .*pool peak" $L | sort | uniq -c | head -50 | cut -c1-260; cat ~/ofimem17/R/log/ctr_diff.txt'
  $R 'grep -E "^mem|devs|MEM_REPORT|hipMalloc|vslot|v-slot" ~/ofimem17/R/log/n10_record.log | cut -c1-300 | head -150'      # part C
  $R 'cat ~/ofimem17/R/log/nodes_start.txt; grep -E "^plan (check|pool)|^room:" ~/ofimem17/R/log/plan_n10.txt ~/ofimem17/R/log/layout_n10.txt | cut -c1-300'
  ```
- **Then:** fill §3–§4 below and RESULTS §108 (B and C rows), compare with SHMEM try 2 (3532.05 s; comm-mark per level: tree 1
  1.05 GB/s, tree 2 0.75, reciprocal 1.38, division 0.61 — results/RUN16.md §6), and do part C from the `MEM_REPORT_DEVS` lines.
  If the verdict is BLOCKED (marker or steps), re-arm with the same command:
  `cd ~/ofimem17/ecalc && setsid nohup bash tests/std17_drive.sh > ~/ofimem17/R/log/driver.out 2>&1 < /dev/null &`.

## 1. Part A — memory accounting under `COMM_OFI` (design)

Before: under `COMM_OFI` (the default on cxi since `e41b47d`) every device exchange stages in the per-device comm pool
(`comm_ofi_alloc`), but the SHMEM pool kept its full staging size and the comm pools (`COMM_SHMEM_POOL_MB`/4 + 256 MiB each) sat
beside it, uncounted by `binsplit_node_bytes`, `as_room_fits`, the `plan pool` line and `mem_model.py`. What SHMEM still carries under
OFI (comm_shmem.c): the control blocks / rings / signal words, the mailbox, and the host-buffer ops (`alltoallv_host`,
`allgather_host`: `set_xo(p, 0)`; only `mn_allgather`'s few words and the OFI blob exchange in ecalc). The smallest correct change:

- `comm_ofi_planned()` (comm_ofi.c): the decision `comm_ofi_enabled` will make, opening nothing (COMM_OFI, else a cxi NIC here, else
  `COMM_OFI_PLAN_CXI=1`); 0 in a build without libfabric.
- `binsplit_shmem_pool_need` under OFI: SHMEM need = 128 MiB (host-op staging; the old floor) + control + mailbox + 256 MiB; the comm pool
  per device = max(one APU's staging, 128 MiB) + its `DIST_MN_SYM_SLABS` slabs + 256 MiB, in whole 256 MiB.
- `binsplit_shmem_pool_rule` under OFI: an unset `COMM_SHMEM_POOL_MB` becomes the SHMEM need (not the 8192 default), no staging
  regions (`COMM_SHMEM_STAGE_SLOT_MB` not exported), `COMM_OFI_POOL_MB` exported as the need when unset (a lower hand value warns); the
  `plan pool` line adds `the comm_ofi pools 4 x COMM_OFI_POOL_MB=…`.
- `as_shmem_pool(…, &ofi)` → `as_room_fits` and `binsplit_node_bytes` add the four comm pools to the host term; the `room:` line prints
  `ofi_pool`.
- `mem_model.py`: `ofi_planned()`, `shmem_pool(ofi=)`, `mem_per_node(ofi=None → as the code decides)`, `--check-c` compares
  `room ofi pools`.
- `mnrun.sh`: the plan runs on the login node, which on aac7 (uan1) has **no cxi** — so with `COMM_OFI` unset it probes the first compute
  node once (`srun --overlap test -e /sys/class/cxi/cxi0`) and exports `COMM_OFI_PLAN_CXI`.
- Docs: README rows (`COMM_OFI_POOL_MB`, new `COMM_OFI_PLAN_CXI`), docs/code/07_COMM_OFI.md §4/§5, 02_PIPELINE_MEMORY.md §3.7,
  docs/TARGET.md launch line (`COMM_SHMEM_POOL_MB=1536`, heap 2048M).

## 2. Part A — tests (all on aac7)

| test | result |
|---|---|
| `MN_PLAN_ONLY=52760000000000:576` (target launch env + `DM_MN_LEAN=1`), login node | COMM_OFI=0: `COMM_SHMEM_POOL_MB=9472` (staging 8192 = 4 × 2048 by tree level of 2, control 894.2 MiB), heap ≥ 9984; **COMM_OFI=1: `COMM_SHMEM_POOL_MB=1536`, heap ≥ 2048 MiB, 4 × `COMM_OFI_POOL_MB=2304`** (measured C output) |
| `BS_LAYOUT_ONLY=9.1597e10:576,9.1597e10:2,5e10:2` + `mem_model.py --check-c … 31 1024 128 0.16 auto`, COMM_OFI 0 and 1 | **exact** both (0 terms not exact; 12 plane figures exact; `room ofi pools` C = model) |
| target node (5.276e13 / 576, `DM_MN_LEAN=1`, the `room:` line = `tests/dl15_ceilings.py`) | no OFI 446.16 GB; **OFI before OFIMEM 457.2 GB** (446.16 + 4 × 2624 MiB uncounted; with the launch line's hand-set 9472: the same); **OFI after 447.50 GB** (SHMEM 1536 MiB + 4 × 2304 MiB); keeping `COMM_SHMEM_POOL_MB=9472` by hand would give 455.8 GB — hence the TARGET.md edit (all modelled; ≤ 480 GB) |
| 10-node layout (8.1e11, LINE10) | SHMEM-only 419.59 GB; OFI before OFIMEM ≈ 430.0 GB (uncounted, over RUN16's 427 GB budget); **OFI after 420.66 GB** (SHMEM 512 MiB + 4 × 2304 MiB) (modelled) |
| 1 node, 2 node-processes, job 12294 (`mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI_VERBOSE=1 POOL_LOG=27 ECALC_VERBOSE=2 ./ecalc {1e8,1e9}`, COMM_OFI unset) | rc 0, `all 2 nodes: VERIFY OK`, `digcmp.sh` **identical** to ref/e_{1e8,1e9}.txt (7.95 s / 13.25 s); mnrun's probe: `COMM_OFI_PLAN_CXI=1`; SHMEM pool 512 MiB, **peak 2 MiB (control 2, staging 0)**; comm pools 512 MiB each, **peak 106.0 MiB** at 1e9 (model's per-APU staging 211.9 MiB); every cxi0–3 carried 4.0–4.2 GB (measured; note 12294's node is CPX — four partitions of one MI300A, RESULTS §107 — so this checks the pools and digits, not memory scale) |

## 3. Part B — the standard 10-node run with all NICs

Pending (RESUME). SHMEM baseline (RUN16 try 2, measured): 8.1e11, `total` 3532.05 s; comm-mark per APU thread: tree level 1
1.05 GB/s, tree level 2 0.75, reciprocal 1.38, division 0.61.

## 4. Part C — B7, the general-map v-exchange slots

Pending the run's `MEM_REPORT_DEVS` lines (`need_vslot` → `hipMalloc`, comm_layered.c:467-474; g = 10 is a general map).

## 5. Open items

- The comm pool's model is ≈ 2 × the measured peak (211.9 vs 106 MiB at 1e9 / 2 procs), as the SHMEM law was: conservative.
- `COMM_OFI_PLAN_CXI` depends on mnrun.sh's probe; a launch without mnrun.sh on a cxi-less login node plans the SHMEM-only pool
  (`COMM_SHMEM_POOL_MB` set from that plan counts as set by hand: the run keeps that SHMEM pool beside the comm pools, the pre-OFIMEM
  footprint — safe but not lean). The target launch line sets `COMM_OFI=1`, so its plan is right on any host.
