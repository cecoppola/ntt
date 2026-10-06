# OFI17 — `comm_ofi`: a multi-NIC bulk path for ecalc (Phase 17, branch `p17-ofi`)

## HANDOFF (2026-10-06, the integrator stopped here — context limit; a fresh agent finishes)

**Merged and pushed to `main`** (`git log --oneline` on `main`, newest first):
- `2afd415` — `mnaccept.sh`/`e16_headline.sh`/`ofi17_drive.sh`: their `e_1e11.out` reference default moves to `~/ref`
  (prep for step 4's aac7 cleanup; `e_4e10.out` is untouched, out of scope).
- `e41b47d` — **`COMM_OFI` now defaults to 1 wherever a cxi NIC is present** (the user's decision, 2026-10-06);
  `COMM_OFI=0`/`=1` still force either path explicitly. Also folds in RUN16.md/OFI17.md's measured write-ups and
  RESULTS.md §105–107.
- `7611751` — the three-way merge of this integration's step 1 (`p17-fix2`, `p17-fix1`, `p17-ofi`, in that order,
  `--no-ff`), the starting point for everything above.

**Verified on aac7 (merged main, commit 7611751e, clone `~/ntt-acc`, built with `SHMEM_CRAY=1 GMP_HOME=~/gmp` +
`make -C ../tools`):**
- (a) `t_edge vmm 4 560` vs `t_edge dev 4 560`, alone on job 12287's SPX node `x9000c1s3b0n0`: **done** — VMM edge
  444.0 GB, dev edge ≈ 448.0 GB (kernel OOM-killed, not a graceful `hipMalloc` failure); the two agree within one
  4 GB step. Written up in RESULTS §107.
- (b) `mnaccept --only unit,e9` on job 12287 (default `COMM_INIT_EARLY`, private `MNACCEPT_TMP=~/acc_verify2_tmp`):
  **not finished when this agent stopped** — 8 of 11 steps passed (`t_ntt`, `t_mul`, `t_bs`, `t_dbig`, `t_newton`,
  `t_verify`, `t_out`, `t_patch`, all identical), `t_mn_grid` (2 node-processes) was still running (progressing
  normally through its growing shapes, ~0 of its 1200 s step timeout exhausted when last checked — not stuck; GPU
  was idle between logged shapes, which is this test's normal pattern, not a hang). `e9` both limb bases have not
  run yet. **It is running detached** (`setsid nohup`, PID 2330564 on the aac7 login node) and writes its own
  summary continuously to `~/ntt-acc/ecalc/results/mnaccept/12287/summary.txt` *and* to `/tmp/acc_unit_e9.log`
  (redirected at launch, both update live and survive this agent's session ending) — collect with:
  ```
  sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com 'cat /tmp/acc_unit_e9.log; pgrep -af "mnaccept.sh 12287"'
  ```
  If the process is gone and the log ends with a `== N passed, M failed …` line, it finished — check all 11 are
  PASS. If it ended without that line (e.g. the 1200 s `t_mn_grid` timeout), diagnose and rerun just the remainder:
  `cd ~/ntt-acc/ecalc && MNACCEPT_TMP=~/acc_verify2_tmp ./mnaccept.sh 12287 --only unit,e9` (safe to rerun whole).
- (c) **not started**: `mnaccept --only mn` at 2 nodes on job 12287, *with the new default* (do **not** set
  `COMM_OFI` — the point is to confirm the just-merged default turns it on by itself on this cxi-equipped node) —
  digits must be identical to the SHMEM baseline. Suggested command once (b) is clean:
  ```
  cd ~/ntt-acc/ecalc && MNACCEPT_TMP=~/acc_verify3_tmp MNRUN_NODES=2 ./mnaccept.sh 12287 --only mn
  ```
  (`COMM_OFI` left unset; `COMM_OFI_VERBOSE=1` would confirm in the log that it actually engaged — expect cxi
  writes, not a plain SHMEM baseline run, since this node has cxi NICs present).
- **Remaining after (b)/(c) pass**: fold their results into RESULTS §106/§107 (replace "a fresh agent will
  confirm" language, if any is added, with the measured PASS lines), then proceed to step 3's remaining write-up
  only if anything in RUN16.md/OFI17.md/RESULTS.md is still open (as of this handoff, §105–107 and RUN16.md/OFI17.md
  are already complete and pushed — re-read them first to confirm nothing drifted), then **step 4** (aac7
  cleanup: `mkdir ~/ref; mv ~/ntt/ecalc/results/e_1e11.out ~/ref/; rm -rf ~/ntt` — the three scripts above are
  already updated for this; still TODO: confirm no other live-used script references the old path, then do the
  move + delete; also delete part files / `*.out` data > 100 MB under `~/p16/R16`, `~/ofi17`, `~/nic16`,
  `~/fix17a`, `~/fix17b` — **already checked by this agent: none exist over 100 MB in any of those five trees**
  (largest single files were 11 MB test binaries); nothing to delete there, just confirm again before reporting
  "freed 0 bytes" to the user).
- **Never touched / still running, do not cancel**: jobs 12287 and 12294 (both must stay up); aac7 clone
  `~/ntt-acc` (building/testing in progress, same clone the mnaccept run above is using — do not `git fetch`/
  rebuild it while that PID is still alive).

Agent report (original, unchanged below). Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled
**measured** / **modelled** / **assumed**. Design: docs/code/07_COMM_OFI.md. Method: results/NIC16_experiments.md.

## Status (updated 2026-10-06, the integrator)

**Adopted.** `COMM_OFI` now defaults to 1 wherever a cxi NIC is present (the user's decision of 2026-10-06,
after the multi-node results below); `COMM_OFI=0` restores the old SHMEM-only path exactly. See RESULTS §106,
`ecalc/README.md`'s `COMM_OFI` row and docs/code/07_COMM_OFI.md. The RESUME block below is left as the agent
wrote it (it predates the decision and still says "off by default").

## RESUME (updated 2026-10-06 01:05 EDT)

- **Built** on branch `p17-ofi` (not merged): `ecalc/comm_ofi.c`, `ecalc/comm_ofi.h`, hooks in `ecalc/comm_shmem.c`, Makefile
  (`OFI=1` automatic with `/opt/cray/libfabric/2.3.1`), README rows, `ecalc/tests/ofi17_drive.sh`. Switch `COMM_OFI=1`, off by default.
- aac7 clone: `~/ofi17` (pull from GitHub, `make -s -j48 SHMEM_CRAY=1` after `source ecalc/aac7env.sh`; tools built too).
- **Unit (1 node, job 12294) passed** — see §2.
- **Armed** (fire-and-forget): `~/ofi17/ecalc/tests/ofi17_drive.sh 12287` (nohup, PID 2042505 on the aac7 login node) waits for
  `~/p16/R16/RUN16_DONE` AND `~/nic16/2N_DONE` (deadline 09:00 EDT), then for job 12287's other steps to end, then runs:
  tc2 (t_comm at 2 nodes, OFI 1/0) → acc (mnaccept unit,e9,mn, `COMM_OFI=1 MNRUN_NODES=2`) → bw2 (t_comm --bw 2 nodes, both) →
  ab (1e10 per node at 2 and 4 nodes, OFI 0/1 interleaved, 2 each, `MN_COMM_MARK=1`) → bwn (t_comm --bw at 4, 8, 10 nodes, both).
  Writes `~/ofi17/R/drive/DONE` (first line = verdict), `drive.log`, `walls.txt`, logs.
- Also queued: `~/ofi17/acc1.sh` (1-node `mnaccept --only mn` with `COMM_OFI=1` on 12294 once the other agent's mnaccept there,
  PID 2027981, exits) → `~/ofi17/R/acc1b_summary.txt`, `~/ofi17/R/ACC1_DONE`.
- **Collect**:
  ```
  R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com"
  $R 'head -1 ~/ofi17/R/drive/DONE; cat ~/ofi17/R/drive/drive.log | cut -c1-300 | tail -80; cat ~/ofi17/R/drive/walls.txt'
  $R 'cat ~/ofi17/R/acc1b_summary.txt'
  $R 'ps -eo pid,etime,args | grep -E "[o]fi17_drive|[a]cc1.sh"'     # kill only by these PIDs
  ```

## 1. Design (summary; full: docs/code/07_COMM_OFI.md)

- One process per node, the in-process xGMI stage, Cray SHMEM for init / control words / barriers / host collectives: unchanged.
- Under `COMM_OFI=1` the *data* of every device exchange of comm_shmem.c (alltoall, alltoallv, allgather, the rounds) goes as
  `fi_writemsg(FI_DELIVERY_COMPLETE)` chunks (4 MiB, window 64 per NIC) over the NICs of the calling APU thread's device (one
  libfabric `cxi` fabric/domain/EP/CQ/AV per NIC, a per-thread list: `COMM_OFI_NICS="0,4;1,5;…"` for 2 NICs per APU). Once all
  chunks to a peer completed, the existing SHMEM signal word is put; the receiver's protocol is untouched.
- Registered memory: a comm pool per device (`COMM_OFI_POOL`: fine-grained device memory by default, hipMalloc, or NUMA-local
  host), sized `COMM_SHMEM_POOL_MB/4 + 256` MiB; staging and `comm_sym_alloc` of OFI communicators come from it.
- Integration: a data plane inside comm_shmem.c (a per-exchange base pointer `db`, ~60 changed lines) + comm_ofi.c (≈ 330 lines).
  Not a new comm_ops: it reuses the sequences, roff publication, rounds, counts check and staging logic.
- Endpoints: 4 × k per process, independent of the number of communicators.

## 2. Unit tests (1 node, job 12294, Cray OpenSHMEMX + libfabric 2.3.1, all measured)

| test | command (in `~/ofi17/ecalc`, `COMM_SHMEM_POOL_MB=1024`, `SLURM_JOB_ID=12294 MNRUN_NODES=1`) | result |
|---|---|---|
| t_comm 2 PEs, OFI | `mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_VERBOSE=1 ./tests/t_comm` | VERIFY OK ×2; data on cxi0 (31–32 writes, 0.05–0.06 GB per PE) |
| t_comm 4 PEs, OFI (strided sets → devices 1, 2 → cxi1, cxi2) | same with 4 | VERIFY OK ×4; cxi0, cxi1, cxi2 carry the writes |
| rounds | + `COMM_SHMEM_ROUND_MB=1 COMM_OFI_CHUNK_MB=0.25` | VERIFY OK ×4; 42 exchanges through the rounds' path, 18 in 285 rounds |
| 2 NICs per device, small window | + `COMM_OFI_NICS="0,1;1,2;2,3;3,0" COMM_OFI_CHUNK_MB=1 COMM_OFI_WINDOW=4` | VERIFY OK ×4; device 1 stripes over cxi1 + cxi2; all 4 NICs carry writes |
| baseline | `COMM_OFI=0`, 4 PEs | VERIFY OK ×4 |
| ecalc 1e8 at 2 node-processes, OFI (HIP build: fine-grained device pools registered with `FI_HMEM_ROCR`, ROCm 7.2.4) | `mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI=1 POOL_LOG=27 ./ecalc 100000000 <f>` | VERIFY OK both; `digcmp.sh` **identical** to ref/e_100000000.txt; each device 0.42–0.43 GB in ≈ 760 writes on its own cxi |

Bug found and fixed on the way: the cxi provider returns `mr_mode` without `FI_MR_VIRT_ADDR` (offset-based RMA addresses); the
first run died with `ADDR_OUT_OF_RANGE`. The blob now carries base 0 in that case (commit cdf1043).

Traps met: (1) another agent (`~/fix17a`) runs `mnaccept` in job 12294 at the same time and both default to
`MNACCEPT_TMP=~/p16/mnaccept_tmp` (`rm -f $TMP/*` at start) — use a private `MNACCEPT_TMP`; (2) a fresh clone needs `make -C
../tools` (digcmp.sh uses `../tools/unpack_digits`; without it every comparison prints DIFFERS).

Per the coordinator's constraint, no bandwidth sweep ran before `RUN16_DONE` (the 10-node production run in 12287).

## 3. Multi-node (job 12287, `~/ofi17` at f26da31, all measured)

**tc2** (`t_comm` at 2 nodes): `COMM_OFI=1` rc 0, 2/2 VERIFY OK, cxi0 0.05–0.06 GB in 31–32 writes per PE;
`COMM_OFI=0` rc 0, 2/2 VERIFY OK (baseline, no OFI counters).

**acc** (`mnaccept --only unit,e9,mn`, `COMM_OFI=1 MNRUN_NODES=2`): **16 passed, 0 failed, 1093 s** — every unit
test, both `e9` limb bases, and `mn` at sizes 2/3/4 (10⁸) and 2/4 (10⁹) node-processes, each spread to that
many nodes by `mnrun.sh`'s default placement (nn = min(allocation, procs), so size 2 already means 2 nodes);
every size identical to the reference.

**bw2/bw4/bw8/bw10** (`t_comm --bw`, 4 APU threads/node, aggregate GB/s across all per-NIC counters at the
4 MiB slab, SHMEM vs OFI):

| nodes | SHMEM (4 MiB) | OFI (4 MiB) | OFI NIC spread (vs SHMEM's 1 NIC/node) |
|---|---|---|---|
| 2 | 19.83 GB/s | 25.01 GB/s | cxi0 only (shmem) vs all 4 cxi (ofi), 0.56 GB/node each |
| 4 | 14.92 GB/s | 48.51 GB/s | same pattern, 0.42 GB/node each |
| 8 | 11.71 GB/s | 44.44 GB/s | same pattern, 0.98 GB/node each |
| 10 | 12.29 GB/s | 44.11 GB/s | same pattern, 1.27 GB/node each |

SHMEM uses one NIC (cxi0) per node regardless of node count (Cray OpenSHMEMX's one-NIC-per-PE binding,
docs/TARGET.md trap 18) and its aggregate falls as more nodes compete for that one path per node (19.8 → 11.7
GB/s from 2 to 8 nodes); OFI stripes every exchange over all 4 NICs per node and holds 44–48 GB/s from 4 nodes
on, 2.3–3.8× the SHMEM aggregate at the same node count.

**ab** (full `ecalc` runs, 1 × 10¹⁰ digits per node, `MN_COMM_MARK=1`, SHMEM vs OFI, 2 tries each, all VERIFY
OK and digit-identical to the baseline / the 10¹¹ prefix):

| nodes | digits | SHMEM `total` (2 tries) | OFI `total` (2 tries) | OFI speedup |
|---|---|---|---|---|
| 2 | 2 × 10¹⁰ | 86.39 s, 86.02 s | 75.27 s, 76.23 s | −12–13 % |
| 4 | 4 × 10¹⁰ | 116.12 s, 120.72 s | 87.26 s, 94.39 s | −22–25 % |

**bwn** (`t_comm --bw` at 4/8/10 nodes): see the bw2/bw4/bw8/bw10 table above (one driver run covered 2–10
nodes).

**Verdict: adopted** (RESULTS §106) — every multi-node test (`tc2`, `acc`'s unit/e9/mn, the four `ab` full runs)
is digit-identical between `COMM_OFI=0` and `=1`, and OFI is faster everywhere measured: −12–25 % on full
`ecalc` runs at 2–4 nodes and 2.3–3.8× the raw aggregate bandwidth at 4–10 nodes once SHMEM's one-NIC-per-node
cap starts to bind.

**NIC16's own 2-node confirmation superseded**: `~/nic16/2N_results.txt` shows every `nicbw`/`nicbw_g`/`nicbw_s`
multi-NIC probe failing at `srun --gres=gpu:24`: "Invalid generic resource (gres) specification" — `--gres=gpu:N`
asks per-task GPU shares that this SPX partition does not grant that way (it grants whole GPUs; `--gres=gpu:24`
is a CPX-partition idiom). Only the single-NIC `shbw` fallback ran there (22.0 GB/s/PE, 44.0 GB/s total at 2
nodes × 1 PE/node). The `t_comm --bw` driver above (bw2/bw4/bw8/bw10) does not need `--gres` and ran clean at
2–10 nodes with every NIC exercised — it supersedes the NIC16 2-node attempt.

## 4. Open items

- The SHMEM pool keeps its full size under `COMM_OFI=1` while the device staging moves to the comm pool: the node holds both
  (≈ the SHMEM pool again per node). Shrinking it needs the pool rule (`binsplit_shmem_pool_rule`, mem_model) to know about OFI.
- The budget check / mem_model do not count the comm pool.
- Two NICs per APU are exercised only as two NICs of different NUMA nodes per device on aac7 (one NIC per APU there).
