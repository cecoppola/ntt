# SHM - the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT)

**Question.** Why does a 6-node run at 1e10 digits with `COMM_OFI=0` (SHMEM carries the device exchanges) stop after
init on every rank with rc 3, "the symmetric pool (2816 MiB) cannot hold 87 MiB more (in use 372 MiB: control 24,
staging 0, symmetric buffers 348)" (results/NIGHT.md, known failure A), when 2 nodes pass; and what pool does the planner
have to give `COMM_OFI=0` at every node count?

**Change / branch.** Branch `shm` (commits c5aae92c, 30891e2b and the docs commit). No new switch: the pool size is a
model output, digits cannot change. `COMM_OFI=1` (the default transport) is unchanged.

- `ecalc/binsplit.c` `binsplit_shmem_pool_need`: under `COMM_OFI=0` the v-slots of the four APU threads are added to the
  pool (`sym += 4 x vsp`) also when `COMM_LAYER_VSLOT_RELEASE=1` (the default); before, M3 (results/C06.md) took
  `max(staging, 4 x vsp)`. The `plan pool` line now names the slots (`v-slots <MiB> = 4 x <MiB>`).
- `ecalc/mem_model.py` `shmem_pool`: the same law.
- `ecalc/README.md`, `COMM_LAYER_VSLOT_RELEASE` row: the "larger, not the sum" rule holds under `COMM_OFI` only.

**Cause.** comm_shmem cuts the pool into four staging regions at its end, one per APU, each the model's per-APU staging
(`COMM_SHMEM_STAGE_SLOT_MB`, set by the pool rule; added against fragmentation), and a general area in front. Staging
blocks go to their APU's region; the v-slots (`comm_sym_alloc`, kind "symmetric buffers") come from the general area
only. The pool rule still used M3's "staging and slots share the pool in time" law, `max(4 x staging, 4 x slots)`, which
no longer holds once staging is in its own regions: the general area was left at control + mailbox + the 256 MiB margin.
At 6 nodes, 1e10 (m, the failing log): pool 2816 MiB = 4 regions of 591 MiB + a general area of 452 MiB; 8 slots of
87 MiB (one pair per APU thread, `COMM_LAYER_VSLOT_MB_6=87`) + 24 MiB control need 720 MiB, so the 5th slot (ranks 0-3)
or the 6th (ranks 4-5, 16 MiB control) found no room. At 2 and 4 nodes the general map has no group that is not a power
of two, so there are no v-slots (0 MiB, mod) and the bug cannot show; it hits every node count with a cut group (3, 5, 6,
..., 576). Not a split per thread, a cap or alignment: a model law that the region layout had made wrong. mnrun.sh was
right: it hands the command's `COMM_OFI=0` to the planner, which sized the pool for `COMM_OFI=0`.

**Method.** Planner: `MN_PLAN_ONLY=<digits>:<nodes> ./ecalc <digits>` and `tools/plan_values.py` (the target template
with `__FILL__` replaced by placeholders) on the aac7 login node, builds of `origin/main` b1e4d567 (base) and of the
branch (fix), both `make -B SHMEM_CRAY=1` with `-DCOMM_SHMEM`. Runs: aac7 queue item `50_shm_pool.sh` (6-node hold),
launch line of `profiles/aac7.conf` + `COMM_OFI=0`, via `mnrun.sh` (pool from the planner): base at 6 nodes, 1e10 (the
reproduction); fix at 6, 4 and 2 nodes, 1e10, digits written and compared with the first 1e10 digits of the 1e11
reference (`tools/unpack_digits` | `cmp -n`); then `tools/gate.sh --build --nodes 2` and `tools/gate.sh --nodes 1`
(defaults, the aac7 line). Collect: `ssh <cluster> 'cat ~/shm/summary.txt'` (access: see the private protocol).

**Results.**

Pool per node-process from the planner (MiB, mod; the pool is host memory, the heap is the pool + 512):

| digits, nodes | `COMM_OFI=0` base | `COMM_OFI=0` fix | of which v-slots (fix) | `COMM_OFI=1` (both) |
|---|---|---|---|---|
| 1e10, 2 | 4608 | 4608 | 0 | 512 |
| 1e10, 4 | 3584 | 3584 | 0 | 512 |
| 1e10, 6 | 2816 | 3328 | 688 = 4 x 172 | 512 |
| 3.82e13, 576 (target template) | 10240 | 17664 | 7312 = 4 x 1828 | 2304 |

At the target the `COMM_OFI=0` pool grows by 7424 MiB (7.25 GiB) per node (mod); `plan check` stays OK (1240 products).
The base value 10240 MiB at the target (results/TGT.md) would have failed the same way: a general area of about 2 GiB
for 1.6 GiB of control and 7.1 GiB of slots (mod).

Runs (aac7, m): PENDING -- queue item `50_shm_pool.sh`, results in `~/shm/summary.txt`.

| run | nodes | rc | VERIFY | digits | total |
|---|---|---|---|---|---|
| base `COMM_OFI=0` (reproduction) | 6 | | | not written | |
| fix `COMM_OFI=0` | 6 | | | | |
| fix `COMM_OFI=0` | 4 | | | | |
| fix `COMM_OFI=0` | 2 | | | | |
| `gate.sh --build --nodes 2` (defaults) | 2 | | | | |
| `gate.sh --nodes 1` (defaults) | 1 | | | | |

**Verdict.** PENDING the runs. Merge when the four fix rows and both gates pass.

**Open issues.**
- Memory cost under `COMM_OFI=0`: the slots and the staging no longer share space (+7.25 GiB host per node at the target,
  mod). Getting M3's sharing back means letting a thread's slots use its own staging region (they are not live at the
  same time under `COMM_LAYER_VSLOT_RELEASE=1`); that would be a behaviour change behind a switch, not done here.
- The model's control (14 MiB at 6 nodes) is below the measured in-use control (24 MiB); the 256 MiB margin covers it.
- 3 and 5 nodes (cut groups, slots present) not run.

**RESUME.** Branch `shm` (local worktree `../ntt-wt-shm`; aac7 worktrees `~/ntt-wt/shm` and `~/ntt-wt/shm-base`,
outputs in `~/shm/`). Queue item `50_shm_pool.sh` writes `~/shm/summary.txt` and the marker `~/shm/DONE`. Next: read the
summary, fill the runs table and the verdict, doccheck 0, merge `shm` to `main`, push, sync aac7 `~/ntt-acc`, remove the
worktrees (local and both on aac7) and `~/shm/bin`, check no job steps are left.
