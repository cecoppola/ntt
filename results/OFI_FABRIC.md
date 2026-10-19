# OFI_FABRIC — comm_ofi (libfabric multi-NIC path), NIC use, the fabric limit and the exchange rate versus the NIC rate

Scope: comm_ofi (libfabric multi-NIC path), NIC use, the fabric limit and the exchange rate versus the NIC rate. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| OFI17 | `comm_ofi`: a multi-NIC bulk path for ecalc (Phase 17, branch `p17-ofi`) |
| STD17 | OFI memory accounting (OFIMEM) and the standard 10-node run with all NICs (Phase 17, branch `p17-ofimem`) |
| TUNE17 | comm_ofi tuning: chunk size × writes in flight, and two NICs per device (Phase 17, aac7) |
| NIC16_experiments | NIC16 — one process driving all Slingshot NICs of a node (aac7) |
| P16 | Phase 16 P: the fabric limit as an algorithm-design problem on aac7 — four PEs per node |
| C02 | (opportunity C8): the gap between the layered exchange rate and the NIC rate -- where it goes at 4 / 6 nodes, three peer |
| XEFF | why ecalc's exchanges underuse the transport, and what to change |
| T12 | 4-node uffd soak (FI_MR_CACHE_MONITOR=userfaultfd) on aac7 |

## OFI17 — `comm_ofi`: a multi-NIC bulk path for ecalc (Phase 17, branch `p17-ofi`)

(former file: OFI17.md)

#### CLOSEOUT (2026-10-06, a fresh agent finished the handoff above)

**Step (b), `mnaccept --only unit,e9` on job 12287** (commit 7611751e, clone `~/ntt-acc`, private
`MNACCEPT_TMP=~/acc_verify2_tmp`): finished at **10 passed, 1 failed, 2039 s**. The one failure is exactly the
known harness timeout flagged in this handoff: `t_mn_grid` (2 node-processes) hit `rc 124` after its 1200 s
`timeout` wrapper (`srun: Job step aborted: Waiting up to 32 seconds for job step to finish`) — the TCP-between-
2-nodes path this test exercises is known to run slow/stall on this harness, not a regression from this
integration. The other 7 unit tests and both `e9` limb bases (10, 2) were VERIFY OK and identical.

**Step (c), `mnaccept --only mn` at 2 nodes, new default, with a bug found and fixed along the way:**
- First attempt (commit 7611751e, the clone's stale HEAD): with `COMM_OFI` unset, **no OFI engaged** — the
  clone `~/ntt-acc` had never pulled past `7611751e` (the pre-flip merge commit; its own summary line printed
  "COMM_OFI, off by default", the merge's *original* commit message, not e41b47d's text). **Fixed**: rebuilt a
  fresh bundle from local `main` (`git bundle create`), `scp`'d it over `~/ntt-main.bundle`, `git fetch origin
  main && git merge --ff-only` to `62af35a4`, rebuilt (`make -s -j48 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp`). No code
  change — this was a stale-clone problem, not the default logic.
- Second attempt (now on 62af35a4, the correct commit): `MNACCEPT_TMP=~/acc_verify3_tmp MNRUN_NODES=2
  COMM_OFI_VERBOSE=1 ./mnaccept.sh 12287 --only mn`, `COMM_OFI` still unset — **5 passed, 0 failed, 409 s**, all
  digit-identical, but still **no `comm_ofi` line in any per-test log**. Root cause: `mnaccept.sh`'s `mn` step
  never sets `COMM_TRANSPORT=shmem`; `ecalc`'s own default transport (`ecalc/mn.c` line 19) is **TCP**, not
  SHMEM, and `comm_ofi` only hooks into `comm_shmem.c` — so these particular acceptance tests have always run
  over TCP regardless of this phase, and `COMM_OFI`'s new default (which only applies once SHMEM is already the
  active transport) had nothing to turn on. This is why `ofi17_drive.sh`'s own `acc` step (§3 below) wraps the
  whole `mnaccept.sh --only unit,e9,mn` call in `COMM_TRANSPORT=shmem COMM_OFI=1 …` — the handoff's suggested
  command for step (c) omitted that wrapper.
- Third attempt, adding the missing `COMM_TRANSPORT=shmem` (still `COMM_OFI` unset — the actual test of the new
  default): `MNACCEPT_TMP=~/acc_verify3_tmp MNRUN_NODES=2 COMM_TRANSPORT=shmem COMM_OFI_VERBOSE=1 ./mnaccept.sh
  12287 --only mn` — **5 passed, 0 failed, 85 s** (vs 409 s over TCP: the SHMEM+OFI path is also simply faster
  here), all five (`e8` sizes 2/3/4, `e9` sizes 2/4) digit-identical to the reference. Every per-test log now
  shows each of the 4 devices opening its own NIC (`comm_ofi: device 0: … NICs cxi0(numa 0) …` through
  device 3/cxi3) and writing on it at finalize (`cxi0 4.17 GB in 1572 writes`, …, one device per cxi, no
  cross-wiring) — **the new default does turn OFI on by itself** once SHMEM is the active transport, with
  `COMM_OFI` and `COMM_OFI_VERBOSE` both left unset/no-op for the default path. **This confirms the user's
  2026-10-06 decision works as intended**; it does not change `mnaccept.sh`'s `mn` step, which intentionally
  keeps testing the TCP baseline unless the caller opts into SHMEM.

**aac7 cleanup (step 4, this agent):** `mkdir -p ~/ref; mv ~/ntt/ecalc/results/e_1e11.out ~/ref/` (100,000,000,003
bytes); `rm -rf ~/ntt` freed the remaining ≈1.3 GB of that tree. Confirmed `mnaccept.sh`/`e16_headline.sh`/
`ofi17_drive.sh` (already updated, commit 2afd415) are the only *live* scripts that referenced the old path;
several Phase 15/16 batch scripts (`dkm15_batch.sh`, `k15_batch.sh`, `kp15_batch.sh`, `run16.sh`, etc., and
historical `.md` write-ups) still mention `~/ntt/ecalc/results/e_1e11.out` but belong to finished phases and are
not run by anything live — left untouched, out of scope. No part files or `*.out` data over 100 MB exist under
`~/p16/R16`, `~/ofi17`, `~/nic16`, `~/fix17a`, `~/fix17b` (largest files are ~11 MB test binaries) — nothing to
delete there.

Agent report (original, unchanged below). Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled
**measured** / **modelled** / **assumed**. Design: internal code notes. Method: results/OFI_FABRIC.md §NIC16_experiments.

#### Status (updated 2026-10-06, the integrator)

**Adopted.** `COMM_OFI` now defaults to 1 wherever a cxi NIC is present (the user's decision of 2026-10-06,
after the multi-node results below); `COMM_OFI=0` restores the old SHMEM-only path exactly. See RESULTS §106,
`ecalc/README.md`'s `COMM_OFI` row and internal code notes. The RESUME block below is left as the agent
wrote it (it predates the decision and still says "off by default").

#### RESUME (updated 2026-10-06 01:05 EDT)

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
  R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 aac7"
  $R 'head -1 ~/ofi17/R/drive/DONE; cat ~/ofi17/R/drive/drive.log | cut -c1-300 | tail -80; cat ~/ofi17/R/drive/walls.txt'
  $R 'cat ~/ofi17/R/acc1b_summary.txt'
  $R 'ps -eo pid,etime,args | grep -E "[o]fi17_drive|[a]cc1.sh"'     # kill only by these PIDs
  ```

#### 1. Design (summary; full: internal code notes)

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

#### 2. Unit tests (1 node, job 12294, Cray OpenSHMEMX + libfabric 2.3.1, all measured)

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

#### 3. Multi-node (job 12287, `~/ofi17` at f26da31, all measured)

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
internal target notes trap 18) and its aggregate falls as more nodes compete for that one path per node (19.8 → 11.7
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

#### 4. Open items

- The SHMEM pool keeps its full size under `COMM_OFI=1` while the device staging moves to the comm pool: the node holds both
  (≈ the SHMEM pool again per node). Shrinking it needs the pool rule (`binsplit_shmem_pool_rule`, mem_model) to know about OFI.
- The budget check / mem_model do not count the comm pool.
- Two NICs per APU are exercised only as two NICs of different NUMA nodes per device on aac7 (one NIC per APU there).

## STD17 — OFI memory accounting (OFIMEM) and the standard 10-node run with all NICs (Phase 17, branch `p17-ofimem`)

(former file: STD17.md)

Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured** / **modelled** / **assumed**.

#### RESUME (2026-10-06 13:55 EDT) — complete

All three parts done; nothing armed, nothing running (driver exited 13:47 EDT; jobs 12287 / 12294 untouched beyond `--overlap`
steps). Logs on aac7: `~/ofimem17/R/log/` (std17.log, n2_sanity.log, n10_record.log, n10_recheck.log, plan/layout, nodes_start.txt,
ctr_before/after/diff.txt), verdict `~/ofimem17/R/STD17_DONE`. Clone `~/ofimem17` (branch `p17-ofimem`, merged into `main`).
Open: B7's v-slots (§4) are not yet counted in the layout / model.

#### 1. Part A — memory accounting under `COMM_OFI` (design)

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
- `mnrun.sh`: the plan runs on the login node, which on aac7 (aac7-login) has **no cxi** — so with `COMM_OFI` unset it probes the first compute
  node once (`srun --overlap test -e /sys/class/cxi/cxi0`) and exports `COMM_OFI_PLAN_CXI`.
- Docs: README rows (`COMM_OFI_POOL_MB`, new `COMM_OFI_PLAN_CXI`), internal code notes §4/§5, 02_PIPELINE_MEMORY.md §3.7,
  internal target notes launch line (`COMM_SHMEM_POOL_MB=1536`, heap 2048M).

#### 2. Part A — tests (all on aac7)

| test | result |
|---|---|
| `MN_PLAN_ONLY=52760000000000:576` (target launch env + `DM_MN_LEAN=1`), login node | COMM_OFI=0: `COMM_SHMEM_POOL_MB=9472` (staging 8192 = 4 × 2048 by tree level of 2, control 894.2 MiB), heap ≥ 9984; **COMM_OFI=1: `COMM_SHMEM_POOL_MB=1536`, heap ≥ 2048 MiB, 4 × `COMM_OFI_POOL_MB=2304`** (measured C output) |
| `BS_LAYOUT_ONLY=9.1597e10:576,9.1597e10:2,5e10:2` + `mem_model.py --check-c … 31 1024 128 0.16 auto`, COMM_OFI 0 and 1 | **exact** both (0 terms not exact; 12 plane figures exact; `room ofi pools` C = model) |
| target node (5.276e13 / 576, `DM_MN_LEAN=1`, the `room:` line = `tests/dl15_ceilings.py`) | no OFI 446.16 GB; **OFI before OFIMEM 457.2 GB** (446.16 + 4 × 2624 MiB uncounted; with the launch line's hand-set 9472: the same); **OFI after 447.50 GB** (SHMEM 1536 MiB + 4 × 2304 MiB); keeping `COMM_SHMEM_POOL_MB=9472` by hand would give 455.8 GB — hence the TARGET.md edit (all modelled; ≤ 480 GB) |
| 10-node layout (8.1e11, LINE10) | SHMEM-only 419.59 GB; OFI before OFIMEM ≈ 430.0 GB (uncounted, over RUN16's 427 GB budget); **OFI after 420.66 GB** (SHMEM 512 MiB + 4 × 2304 MiB) (modelled) |
| 1 node, 2 node-processes, job 12294 (`mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI_VERBOSE=1 POOL_LOG=27 ECALC_VERBOSE=2 ./ecalc {1e8,1e9}`, COMM_OFI unset) | rc 0, `all 2 nodes: VERIFY OK`, `digcmp.sh` **identical** to ref/e_{1e8,1e9}.txt (7.95 s / 13.25 s); mnrun's probe: `COMM_OFI_PLAN_CXI=1`; SHMEM pool 512 MiB, **peak 2 MiB (control 2, staging 0)**; comm pools 512 MiB each, **peak 106.0 MiB** at 1e9 (model's per-APU staging 211.9 MiB); every cxi0–3 carried 4.0–4.2 GB (measured; note 12294's node is CPX — four partitions of one MI300A, RESULTS §107 — so this checks the pools and digits, not memory scale) |

#### 3. Part B — the standard 10-node run with all NICs (job 12287, measured)

Driver `tests/std17_drive.sh` (armed 11:20 EDT; the marker appeared 11:43; no other steps in 12287 then). MemAvailable at start:
min **441.0 GB** (aac7-n30, s4b0n0, s4b1n0, s6b0n0; the other six 518.5–520.5) → the full size **8.1 × 10¹¹** (81e9 per node),
layout node **420.66 GB** (modelled; 20.3 GB under the minimum; `room:` shmem_pool 512 MiB, ofi_pool 4 × 2304 MiB). Launch line =
RUN16 try 2's LINE10 + `COMM_OFI_VERBOSE=1`; `COMM_OFI` unset (on: cxi). `plan check … OK — 96 products`.

- **2-node sanity** (2e10): rc 0, `total` 78.41 s, VERIFY OK; SHMEM pool peak 2 MiB (control only); comm pools 1536 MiB, peak 1059.6.
- **Record:** rc 0, **`total` 1669.98 s** (elapsed 1679 s), `mn: all 10 nodes: VERIFY OK`. **SHMEM try 2: 3532.05 s → −52.7 %.**

| phase | SHMEM try 2 (RUN16) | OFI (this run) |
|---|---|---|
| init | 128.8 s | 30.5 s |
| bs | 1225.3 s | 262.6 s |
| dm (recip) | 2174.2 s (267.6) | 478.3 s (205.9) |
| dc (the part-file write, NFS, O_DIRECT) | 0.1 s (hidden under the slow dm) | **897.6 s** (now exposed: 54 % of `total`) |
| total | 3532.05 s | 1669.98 s |

comm-mark (per APU thread, summed over 4 × 10 threads; same exchanges and bytes in both):

| level | exchanges, GB | SHMEM GB/s (wall) | OFI GB/s (wall) | ratio |
|---|---|---|---|---|
| tree level 1 | 1380, 599.31 | 1.05 (324.06 s) | **8.05** (114.12 s) | 7.7× |
| tree level 2 | 4904, 2779.33 | 0.75 (1023.72 s) | **5.63** (174.03 s) | 7.5× |
| reciprocal | 6788, 1209.80 | 1.38 (267.65 s) | **7.86** (205.87 s) | 5.7× |
| division | 8404, 4421.32 | 0.61 (1906.51 s) | **4.84** (272.41 s) | 7.9× |

- **All NICs:** the cxi hardware counters (`hni_sts_tx_ok_octets`, before/after the record) moved on **all 40 NICs (4 × 10 nodes),
  2471.7–2627.2 GB tx each**, 102.7 TB in all (includes the NFS write); `COMM_OFI_VERBOSE` per device and rank: 2.36–2.51 TB on its own
  cxi. SHMEM try 2 used cxi0 only.
- **Pools:** comm pool peak **2048.0 MiB of 2304** on every device of every rank (the model's per-APU staging exactly: the rounds'
  1024 + 1024 MiB; 256 MiB margin unused); SHMEM pool 512 MiB, peak 2 MiB.
- **RECHECK (packed):** rc 0, elapsed 2284 s, `mn: all 10 nodes: RECHECK OK`.
- **1e11 prefix** vs `~/ref/e_1e11.out` (the other agent moved it): **identical** (3387 s); parts deleted afterwards (`~/ofimem17/R` holds logs only, 676 KB). Driver verdict: `SUCCESS`.
- Note: with the fabric 5.7–7.9× faster, the run is now bound by the part-file write to NFS home (897.6 s, ≈ 0.4 GB/s aggregate for
  ≈ 340 GB packed); compute + exchange (bs + dm) fell 3399.5 → 740.9 s (−78 %).

#### 4. Part C — B7, the general-map v-exchange slots (measured from `MEM_REPORT_DEVS`)

`MEM_REPORT_DEVS=1` prints per APU the layout's `in use` and the driver's `used`; the difference is device memory **outside the layout**
(hipMalloc'd: the comm pools, the v-slots, the runtime). Per APU, every rank and APU (measured):

| run | g | driver − in use per APU at [tree] / [released] / [bs] |
|---|---|---|
| this run (OFI) | 10 (general map) | **6.3–6.4 GB** |
| RUN16 try 2 (SHMEM, same size) | 10 | **3.7–3.8 GB** |
| 2-node sanity (OFI) | 2 (power of two: no general map) | 2.0–2.1 GB |

OFI − SHMEM at g = 10 = 2.6 GB ≈ the comm pool (2304 MiB = 2.42 GB) — counted now by OFIMEM. The g = 2 run (comm pool 1.61 GB) leaves
≈ 0.4 GB of runtime per APU; so at g = 10 **≈ 3.3 GB per APU, ≈ 13 GB per node, sits outside the layout and outside the model** —
B7 is **confirmed** (the attribution to `need_vslot`'s grow-only hipMalloc, comm_layered.c:467-474, is inferred: it is the general map's
only such site; the size matches B7's ≈ q·8 per APU estimate). It was already present in the SHMEM run (RUN16's "407 GB measured"
counted only `in use` + host).

Per-node peak vs the layout (rank 0 at [bs], the device peak): driver 4 × 100.9 = 403.6 GB + host RSS 19.9 = **423.5 GB measured
vs 420.66 GB layout (+2.9 GB)**; RUN16 try 2 likewise ≈ 4 × (94.65 + 3.8) + 28.8 ≈ 422.6 vs 419.59. The layout still lands within ≈ 3 GB
only because its host term (44.7 GB: seed buffers 17.2 GB, freed after init) absorbs the ≈ 13 GB; the guard and the 12 GB reserve held
(MemAvailable min 441 GB → ≈ 17.5 GB left at the peak). At the target B7's estimate is ≈ 15 GB per node per general-map level kept
alive (192 and 576 → up to 30): 447.5 → 462.5–477.5 GB (modelled + B7's estimate) — still ≤ 480 but thin. **Recommendation:** count
the v-slots in `rns_mul_dist_mn_scratch` / `mem_model.py` (06_EVALUATION §4.1 item 5), or free them at the end of each level.

#### 5. Open items

- The comm pool's model is ≈ 2 × the measured peak (211.9 vs 106 MiB at 1e9 / 2 procs), as the SHMEM law was: conservative.
- `COMM_OFI_PLAN_CXI` depends on mnrun.sh's probe; a launch without mnrun.sh on a cxi-less login node plans the SHMEM-only pool
  (`COMM_SHMEM_POOL_MB` set from that plan counts as set by hand: the run keeps that SHMEM pool beside the comm pools, the pre-OFIMEM
  footprint — safe but not lean). The target launch line sets `COMM_OFI=1`, so its plan is right on any host.

## TUNE17 — comm_ofi tuning: chunk size × writes in flight, and two NICs per device (Phase 17, aac7)

(former file: TUNE17.md)

Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured** / **modelled** / **assumed**.
Driver: `ecalc/tests/ofitune17_drive.sh` (fire-and-forget). Design of comm_ofi: internal code notes.

#### Why

comm_ofi's all-to-all holds 44–48 GB/s per node at 4–10 nodes (**measured**, results/OFI_FABRIC.md §OFI17 §3), about 11–12 GB/s per NIC,
while one pair of NICs reaches 23.3 GB/s (**measured**, results/OFI_FABRIC.md §NIC16_experiments). The question: is the gap the knobs
(write size `COMM_OFI_CHUNK_MB`, default 4; writes in flight per NIC `COMM_OFI_WINDOW`, default 64) or the fabric
(many-to-many contention)? And does the two-NICs-per-device form (`COMM_OFI_NICS`, the target's `0,4;1,5;…`) work, and at what cost
on aac7, which has one NIC per APU (so the second NIC is a neighbor's, across NUMA)? No new switches; only the existing env knobs.

#### Plan (what the driver runs, in order)

Gate: starts only after `~/ofimem17/R/STD17_DONE` exists (the timed 10-node STD17 production run in job 12287 — no fabric traffic
may overlap it), then waits ≤ 1 h for job 12287's other steps to end. If the marker is not there by **16:00 EDT** the verdict is
`SKIPPED`. It **always** writes `~/ofitune17/DONE` (first line = verdict). Never cancels 12287 / 12294.

| step | what | nodes |
|---|---|---|
| sw4 | `t_comm --bw 4 5 16` (all-to-all, 4 threads/node, slabs 64 KiB–16 MiB), `COMM_OFI_CHUNK_MB` ∈ {1, 4, 8} × `COMM_OFI_WINDOW` ∈ {16, 64, 128} (9 runs; 4 × 64 = the default) | first 4 of 12287 |
| sw10 | the best two of sw4 (mean per-node aggregate GB/s at the 16 MiB slab) and the default, same test | all 10 |
| nic2 | default NICs `--bw`; then for `COMM_OFI_NICS="0,1;1,2;2,3;3,0"` (A) and `"0,0;1,1;2,2;3,3"` (B, duplicate entries: two domains on one cxi): `t_comm` correctness (2 VERIFY OK), then `t_comm --bw 4 5 64` | first 2 |
| ec2 | `ecalc 2e10` on 2 nodes (ofi17's launch line, `COMM_OFI=1`), default NICs vs the better of A/B: VERIFY OK, parts' sha1 identical, top part identical to the `e_1e11.out` prefix (`~/ref`, else `~/ntt/ecalc/results`) | first 2 |

Every `--bw` run records per-PE per-thread and aggregate GB/s per slab (`<tag>.log`), the four cxi counters per node before/after
(`hni_sts_{tx,rx}_ok_octets`, `drive.log`), and a row in `bw.tsv`: tag, nodes, chunk, window, nics, rc, aggregate at the largest slab
(mean and min over PEs), per-thread mean, aggregate at 4 MiB.

Clone: `~/ofimem17` on aac7 at `main` (the build of `0c77c4c`; `0c77c4c..8e03ba0` and this commit change no code, so no rebuild —
the STD17 driver runs from the same clone's binaries).

#### COLLECT

```
R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 aac7"
$R 'head -1 ~/ofitune17/DONE; column -t -s$'"'"'\t'"'"' ~/ofitune17/bw.tsv; cat ~/ofitune17/walls.txt'
$R 'cut -c1-400 ~/ofitune17/drive.log | tail -120'
$R 'grep -E ": pe [0-9]+ .*16777216 B per slab" ~/ofitune17/sw4_*.log ~/ofitune17/sw10_*.log | cut -c1-260'
$R 'pgrep -af "^bash tests/ofitune17_drive.sh"; squeue -s -j 12287 -h -o "%i %j %M"'     # kill only by that PID
```
Re-arm (e.g. after `SKIPPED`), with a later deadline in epoch seconds:
```
$R 'cd ~/ofimem17/ecalc && TUNE17_DEADLINE=<epoch> setsid nohup bash tests/ofitune17_drive.sh 12287 > ~/ofitune17/driver.out 2>&1 < /dev/null &'
```
A subset: `TUNE17_STEPS=nic2,ec2` (steps sw4, sw10, nic2, ec2; sw10 without sw4 runs the default only).

#### Results

(to be filled from the COLLECT output)

#### RESUME

- **Armed** 2026-10-06 11:38 EDT on the aac7 login node: PID 2394528 (`~/ofimem17` at `31be838`), waiting for `~/ofimem17/R/STD17_DONE` (deadline 16:00 EDT); the STD17 driver (PID 2373608) was itself still waiting for its own marker `~/ofi17/HANDOFF_DONE`, so `SKIPPED` is likely unless STD17 starts and finishes well before 16:00 (its record + RECHECK alone take about 2 h, modelled from RUN16). The verdict lands in `~/ofitune17/DONE`.

#### Results (2026-10-06, measured; driver verdict SUCCESS)

`t_comm --bw` all-to-all, aggregate GB/s per node at the largest slab (mean / min), per-thread GB/s:

| run | nodes | chunk MiB | window | NICs per device | agg mean | agg min | per thread |
|---|---|---|---|---|---|---|---|
| sw4_c1_w16 | 4 | 1 | 16 | 1 | 40.88 | 40.41 | 10.49 |
| sw4_c1_w64 | 4 | 1 | 64 | 1 | 40.98 | 40.53 | 11.15 |
| sw4_c1_w128 | 4 | 1 | 128 | 1 | 40.62 | 40.18 | 10.75 |
| sw4_c4_w16 | 4 | 4 | 16 | 1 | 40.02 | 39.56 | 11.46 |
| **sw4_c4_w64 (default)** | 4 | 4 | 64 | 1 | **46.93** | 46.73 | 12.27 |
| sw4_c4_w128 | 4 | 4 | 128 | 1 | 46.86 | 46.64 | 12.12 |
| sw4_c8_w16 | 4 | 8 | 16 | 1 | 48.07 | 47.98 | 12.74 |
| sw4_c8_w64 | 4 | 8 | 64 | 1 | 46.04 | 45.85 | 11.89 |
| sw4_c8_w128 | 4 | 8 | 128 | 1 | 47.20 | 47.03 | 11.95 |
| sw10_c8_w16 | 10 | 8 | 16 | 1 | 43.52 | 42.87 | 10.97 |
| nic2_default | 2 | 4 | 64 | 1 | 25.72 | 25.71 | 7.72 |
| nic2_formA `0,1;1,2;2,3;3,0` | 2 | 4 | 64 | 2 | 25.67 | 25.66 | 6.72 |
| nic2_formB `0,0;1,1;2,2;3,3` | 2 | 4 | 64 | 2 (same NIC twice) | 24.54 | 24.52 | 6.36 |

- **Tuning:** the best setting (8 MiB, window 16) is +2.4 % over the default at 4 nodes, and at 10 nodes 43.5 vs the default's 44.1 (OFI17), so no gain within the spread. **The defaults (`COMM_OFI_CHUNK_MB=4`, `COMM_OFI_WINDOW=64`) stay.**
- **Two NICs per device:** both forms run correctly, and ecalc 2e10 on 2 nodes with form A gives part sha1s identical to the default (VERIFY OK on both nodes). There is no rate gain on aac7, as expected with one NIC per APU, where form A shares each NIC between two devices. The mechanism is ready for the target's 2 NICs per APU; its rate needs the target (TARGET_WISHLIST §1.3).

## NIC16_experiments — NIC16 — one process driving all Slingshot NICs of a node (aac7)

(former file: NIC16_experiments.md)

Agent report, not committed. Times Eastern. Hardware: aac7 node aac7-n61 (job 12294), 4 × Cassini "SS11 200Gb 2P"
(cxi0..cxi3 = hsn0..3, NUMA 0..3, one per MI300A; cxi0+cxi1 share one card serial, cxi2+cxi3 another), link BS_200G,
MTU 2112, libfabric 2.3.1 (`/opt/cray/libfabric/2.3.1`), no Slurm VNI env (default CXI service, svc_id 1).

#### RESUME (updated 2026-10-05 22:55 EDT)

- **Answer found (measured):** ONE process opening one libfabric `cxi` fabric/domain/EP/CQ/AV per device (`domain_attr->name`
  = "cxi0".."cxi3") drives all 4 NICs at 23.33 GB/s each = **93.3 GB/s per node** (1 MiB `fi_write`, window 64),
  from host memory or `hipMalloc` memory (93.2), with 4 threads **or a single thread** (93.3). 4.0× one NIC.
- Coexists with Cray OpenSHMEMX in the same process (1 PE + 4 extra domains: 93.3 GB/s; SHMEM still works).
- Cray SHMEM and Cray MPICH: one PE / rank = exactly one NIC (man pages; MPICH striping CVARs measured inert).
- Tools/scripts on aac7 `~/nic16/`: `nicbw.c` (copy: `archive:removed_2026-10-12/ecalc/tests/t_nicbw.c`), `build.sh`, `run1.sh`, `ctr.sh`,
  `cdiff.py`, `cxw.sh`, `mpibw.c`, `mrun.sh`. Binaries: `nicbw` (host), `nicbw_g` (HIP, ROCm 7.0.3), `nicbw_s` (+SHMEM).
- Also: remote MR counters work (93.2 GB/s); VMM (`hipMemCreate`) buffers cannot be registered (EFAULT) — register the
  comm pool (hipMalloc / host) instead; 8 domains (2 per NIC) in one process work (90.4).
- **2-node confirmation: running unattended on aac7 (started 2026-10-05 ~23:10 EDT).** `~/nic16/mn.sh` (nohup, PID 1970543)
  waits for `~/p16/R16/RUN16_DONE`, then runs inside job 12287 on its first 2 nodes (`srun --overlap -N2 -n2
  --ntasks-per-node=1 -c 192 --gres=gpu:24`, NIC i → peer NIC i, counters on both nodes via `run2.sh`):
  nicbw 1 NIC uni; 4 NICs uni / bidir; 1-thread uni / bidir; hipMalloc uni; hipMalloc + remote counters; 1 MiB and
  64 MiB writes; hybrid nicbw_s (SHMEM PE + 4 domains); 8 domains; then Cray SHMEM `shbw` 1 PE/node and 4 PEs/node
  (NUMA). Each test has `timeout 120`. `~/nic16/fin.sh` (nohup, PID 1971765) copies the log to
  `~/nic16/2N_results.txt` and writes `~/nic16/2N_DONE` when mn.sh ends.
- **Collect:**
  ```
  R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 aac7"
  $R 'ls ~/nic16/2N_DONE && grep -E "^===|total|TOTAL|counters|cxi[0-3] tx|rror" ~/nic16/2N_results.txt | head -150'
  $R 'pgrep -af "nic16/(mn|fin).sh"'     # still waiting / running?  kill only by these PIDs
  ```
  Pass = 4 NICs uni ≈ 4 × the 1-NIC uni number (≈ 90+ GB/s per node), all 4 NICs' tx counters on the sender node.

#### Counters

`~/nic16/ctr.sh` reads `/sys/class/cxi/cxiN/device/telemetry/hni_sts_{tx,rx}_ok_octets` before/after each run; the
counter GB/s is diluted by setup time (wall includes process start), the program's GB/s is the timed window.

#### Results (all measured, single node, traffic NIC → switch → NIC)

| # | Method | Config | Per NIC GB/s | Aggregate | Counters |
|---|---|---|---|---|---|
| 1 | `cxi_write_bw` 1 pair | cxi0→cxi1, 1 MiB, list 256, 5 s | 24.25 | 24.25 | only cxi0 tx / cxi1 rx |
| 2 | `cxi_write_bw` 2 pairs | cxi0→1, cxi2→3, 1 MiB | 24.25, 24.23 | 48.5 | as expected |
| 3 | `cxi_write_bw` 4-ring | 0→1→2→3→0, 4 processes, 1 MiB | 23.37/21.89/23.36/21.89 | 90.5 | all 4 tx+rx ≈ 19–20 (diluted) |
| 4 | **nicbw 1 process, 4 threads** | ring 0→1→2→3→0, 1 MiB, win 64, host | 23.33 ×4 | **93.32** | all 4: 122 GB tx / 122 GB rx |
| 5 | nicbw 1 process 4 thr | 64 KiB win 256 / 16 MiB win 16 | 23.34 ×4 | 93.35 / 93.34 | |
| 6 | **nicbw 1 process, hipMalloc** (`FI_HMEM_ROCR`) | 1 MiB win 64 | 23.33/23.33/23.20/23.34 | **93.20** | all 4 |
| 7 | **nicbw 1 process, 1 thread** (round-robin over 4 EPs/CQs) | 1 MiB win 64, host | 23.33 ×4 | **93.33** | |
| 8 | nicbw 1 thread, hipMalloc | 1 MiB | 23.34/23.33/23.17/23.34 | 93.18 | |
| 9 | nicbw 1 thread | 64 KiB win 256 | 23.34 ×4 | 93.34 | |
| 10 | nicbw 1 thread | 8 KiB win 512 | 3.95 ×4 | 15.78 (op-rate bound, 1.9 Mops/s) | |
| 11 | nicbw 4 threads | 8 KiB win 512 | 3.39/6.12/6.23/6.22 | 21.95 | |
| 12 | nicbw 1 NIC (cxi0→cxi0 loopback) | 1 MiB | 23.34 | 23.34 | cxi0 only |
| 13 | nicbw **2 processes** × 4 NICs (both share every NIC) | 1 MiB / 4 MiB GPU | 11.7/10.9 per proc-NIC | 45.3 + 45.3 = **90.6** | each NIC 22–23 tx and rx |
| 14 | **hybrid**: `nicbw_s` = 1 Cray SHMEM PE + 4 libfabric domains, same process | 1 MiB win 64 | 23.33 ×4 | **93.34** | all 4; SHMEM putmem afterwards OK (22.4 GB/s to self, via cxi0) |
| 17 | nicbw, hipMalloc + **remote MR counters** (`FI_RMA_EVENT`, `fi_mr_bind(mr, cntr, FI_REMOTE_WRITE)`) | 4 MiB win 16 | 23.31 ×4 | 93.24 | target counters = ops sent (27806 / 27806 …) — completion signalling works at full rate |
| 18 | nicbw, **VMM** buffers (`hipMemCreate`+`hipMemMap`+`hipMemSetAccess`, as ecalc's dbig arena) | — | — | **fails** | `fi_mr_regattr` = −14 EFAULT, "cxil_map: write error"; same with `FI_HMEM_ROCR_USE_DMABUF=0`, with `FI_HMEM_SYSTEM`, with `FI_CXI_ODP=1` |
| 19 | nicbw, `hipHostMalloc` buffers (flags 0 / NumaUser) | 4 MiB win 16 | 23.25/21.9/21.9/21.9 | 89.0 / 88.8 | buffers not NIC-local (cxi0's NUMA) cost ≈ 6 % on NICs 1–3 |
| 20 | nicbw **8 domains** in one process (2 per NIC; target-like) 4 thr / 1 thr | 4 MiB win 16 | 11.7/10.9 per domain | 90.44 / 90.43 | 2 flows per NIC |
| 21 | **Cray SHMEM 4 PEs/node** `shbw` (putmem_nbi+quiet, ring PE→PE+1, `SHMEM_OFI_NIC_POLICY=NUMA`, `-n4 -c 48`) | 4 MiB W16 / 64 MiB W4 / 256 MiB W2 / 1 MiB W64 | 23.24–23.26 | 93.00 / 93.27 / 93.30 / 93.02 | all 4 NICs (intra-node SHMEM goes over the NIC) |
| 22 | nicbw 1 process 4 NICs, large writes | 256 MiB W2 / 64 MiB W4 | 23.33 ×4 | 93.34 / 93.34 | no large-message slump on one node |
| 23 | hybrid `nicbw_s`, buffers = slices of the **Cray SHMEM symmetric heap** (`shmem_malloc`, host hugepages; `NICBW_SHHEAP=1 SHMEM_SYMMETRIC_SIZE=2G`) registered again in the 4 extra domains | 4 MiB W16 | 21.36/21.52/23.34/21.36 | 87.58 | works; heap sits on one NUMA node (cxi2's), the other 3 NICs lose ≈ 8 %. SHMEM putmem afterwards 23.1 GB/s |
| 15 | Cray MPICH 9.1 `mpibw`, 2 ranks 1 node, `MPIR_CVAR_NOLOCAL=1` | 4 MiB, win 16 | — | 23.28 | cxi0 only |
| 16 | same + `MPIR_CVAR_CH4_OFI_ENABLE_MULTI_NIC_STRIPING=1 MPIR_CVAR_CH4_OFI_MAX_NICS=4` (+HASHING, threshold 64 KiB; also `MPICH_CH4_OFI_*` names) | 4 MiB | — | 23.28 | **cxi0 only** — inert |

Commands (on aac7, `~/nic16`):
- #1–3: `srun --jobid=12294 --overlap -N1 -n1 bash -lc "D=6 ~/nic16/cxw.sh cxi1:cxi0:49211 cxi2:cxi1:49212 …"`
  (server `cxi_write_bw -d <dst> -p <port>`, client `numactl -N n -m n cxi_write_bw -d <src> -p <port> -s 1048576 -D 6 localhost`).
- #4–12: `srun --jobid=12294 --overlap --gres=gpu:24 -N1 -n1 -c 192 bash -c "~/nic16/run1.sh ~/nic16/nicbw[_g] [-g] [-1] -n 0,1,2,3 -s 1048576 -w 64 -t 5"`.
- #13: `NP=2 ~/nic16/run1.sh ~/nic16/nicbw -n 0,1,2,3 -x 1 …` (two processes on the node, rank r NIC i → rank r+1 NIC i+1).
- #14: `srun --jobid=12294 --overlap -n1 -c 192 ~/nic16/nicbw_s -n 0,1,2,3 -s 1048576 -w 64 -t 5 -d x -T h1`.
- #15–16: `~/nic16/mrun.sh 2 MPIR_CVAR_NOLOCAL=1 [CVARs]`.

#### The method (exact API)

Per NIC i (device name `cxi<i>`), in one process:
```
hints: fabric_attr->prov_name="cxi"; domain_attr->name="cxi<i>"; ep_attr->type=FI_EP_RDM;
       caps=FI_RMA|FI_MSG(|FI_HMEM); mr_mode=FI_MR_LOCAL|FI_MR_VIRT_ADDR|FI_MR_ALLOCATED|FI_MR_PROV_KEY|FI_MR_ENDPOINT(|FI_MR_HMEM);
       threading=FI_THREAD_DOMAIN; tx_attr->size=4096
fi_getinfo(FI_VERSION(1,20)) → fi_fabric → fi_domain → fi_cq_open(FI_CQ_FORMAT_CONTEXT, 8192) → fi_av_open(FI_AV_TABLE)
→ fi_endpoint → fi_ep_bind(cq, FI_TRANSMIT|FI_RECV), fi_ep_bind(av) → fi_enable → fi_getname
MR: fi_mr_reg (host) or fi_mr_regattr{iface=FI_HMEM_ROCR, device.reserved=gpu} (hipMalloc); FI_MR_ENDPOINT ⇒ fi_mr_bind(mr, ep)+fi_mr_enable
exchange (addr, fi_mr_key, buffer VA) out of band; fi_av_insert(peer addr of the matching NIC)
loop: fi_write(ep, buf, len, fi_mr_desc(mr), peer, peer_va+off, peer_key) up to window W; fi_cq_read to retire
```
No env needed on aac7 (default CXI service). Thread pinned to the NIC's NUMA node (`/sys/class/cxi/cxiN/device/numa_node`).

#### Caveats

- `hipMalloc` in a step needs `srun --gres=gpu:24` (job shows gres/gpu=24); without it /dev/dri is empty. The ROCm 7.2.4
  HIP runtime reported "no ROCm-capable device" in the step where 7.0.3 worked — nicbw_g is linked to 7.0.3.
- Small messages: a single thread is op-rate bound (≈ 1.9 M writes/s total at 8 KiB); ≥ 64 KiB saturates all 4 NICs.
- SHMEM `SHMEM_OFI_NIC_POLICY=NUMA` aborts when the single PE spans all NUMA nodes ("numa node -1"); use default BLOCK.

## P16 — Phase 16 P: the fabric limit as an algorithm-design problem on aac7 — four PEs per node

(former file: P16.md)

Agent P, Phase 16. Branch `p16-P` from `main` dbde0c7. Files: `results/OFI_FABRIC.md §P16`, `ecalc/mnrun.sh` (D's `MNRUN_UNLOAD` / `AAC7_ROCM`
module swap copied from `origin/p16-D` eb499a9 so that ROCm 7.2.4 runs launch; nothing else of mine in it yet). Report times
Eastern (aac7 prints PDT = Eastern − 3 h).

#### 1. Design (on paper; the measurements in §2)

**The problem.** Cray OpenSHMEMX binds one NIC per PE. ecalc's launch form is one PE per node with four APU threads, so the four
SHMEM contexts share one 200 Gb/s cxi NIC: 13.5–14.8 GB/s per node, 3.6 GB/s per APU thread (C16 §2.1, measured), falling to
2.8 / 1.0 at 4 / 8 nodes (B16). With 4 PEs per node every PE lands on its own NIC (the default BLOCK policy; `SHMEM_OFI_NUM_NICS=4`
changes nothing more) and `t_comm` measured 7.6–8.2 GB/s per PE with one context = 31–32 GB/s per node (C16 §2.3, §2.9).

##### 1.1 What in the code assumes "a node-process owns four APUs" (read: mn.c, comm_layered.c, comm_xgmi.c, rns_mul.c, dbig.c)

- `rns_mul.c rns_init`: `hipGetDeviceCount` must return ≥ `EC_NP` = 4 — **prime d lives on device d**; one device per PE
  (`HIP_VISIBLE_DEVICES=i`) is refused at init (`rns_init: need 4 devices, have 1`). The whole engine (planes, the block pools,
  `dbig`'s per-device arenas, `todec`, `spill`) is written as one prime per device.
- `mn.c`: `#define NA 4`; `mn_init` creates one mesh per APU thread d (`comm_shmem_create_at(0, 1, size, d)`: mesh d = APU thread d of
  every PE), `mn_group_at` / `mn_group_span` the same per level; the layered communicator (`comm_layered.c`) takes the global rank space
  as (4 APUs) × (g PEs), rho = g d + r: the intra stage is the xGMI push kernel between the four devices **of the same process**
  (`comm_xgmi_create(d)`: `hipDeviceEnablePeerAccess` over the process's own `hipMalloc` buffers), the inter stage is mesh d over the g PEs.
- `mnrun.sh`: several node-processes per node on aac6 = **each process opens all four APUs with four threads** (nothing is pinned to one
  device; the processes time-share the APUs; `--gpus-per-node=4 --overlap`); correctness only there.

So "one PE = one APU" (g = 4n PEs, NA = 1, prime d on device 0 of PE d mod 4) is **not** a launch-form change: it is a rewrite of
the prime ↔ device binding in rns_mul / dbig / binsplit / the xGMI layer (the intra stage of every all-to-all would move from the
909 GB/s push kernel inside one process to SHMEM between four processes on the node: XPMEM copies for host memory —
`SHMEM_SMP_SIZE_LIMIT` caps them — or `hipIpc` handles exported by hand). **Cost: weeks; not built here.**

##### 1.2 The prototype that needs no code: 4 PEs per node, every PE with all four APUs, the leaves 4 × smaller

`mnrun.sh 4n` on n nodes (block placement: PEs 4r..4r+3 on node r; `mnrun.sh` already does this) with `COMM_TRANSPORT=shmem` on
Cray: PE i of a node gets cxi i (BLOCK). Every PE runs the unchanged engine on all four APUs with its share D/4 of the digits;
the tree has g = 4n leaves (`MN_GROUPS` default: 2, 4, …, 4n — level 1 and 2 pair PEs **of the same node**, their exchanges go over
Cray's on-node transport); each all-to-all's intra stage stays the in-process xGMI push, the inter stage goes over the PE's own NIC
to 4n − 1 peers, 3 of them on the node (XPMEM / loopback — measured in §2.2). The four processes time-share each APU (ROCm's
hardware scheduler) — the compute of the node is the same work as before, split four ways; what changes is the fabric: four NICs
per node instead of one, and a PE that waits on the fabric leaves its APUs to the other three (the oversubscription hides exposed
communication the way multithreading hides latency — the optimistic case). Costs: four processes' fixed memory (the planes are sized
by the largest product of each PE — at D/4 per PE the plan's planes are the same 104 GB per process: `estimate.py` below — so the
node's device memory is the question: §2.1 measures it), four handshakes at init, 4 × the message count per node.

##### 1.3 Modelled walls (`estimate.py --fabric aac7 --write-bw 100`, no-write wall; measured B16/C16 for comparison) — **modelled**

| nodes | 1 PE/node (model, 4 GB/s per APU thread) | measured (C16/B16) | 4 PEs/node, per-PE wall at `--g 4n --D D/4` with `--bw 2` / `--bw 3.5` per thread (8 / 14 GB/s per PE) | the node's wall, pessimistic = init + 4 × (per-PE compute − init) + exposed | optimistic = init + max(4 × compute, exposed + compute) |
|---|---|---|---|---|---|
| 2 × 10¹⁰ | 50.7 (exposed 24.2) | 35.3–36.7 | 40.8 (exp 24.2) / 30.5 (exp 13.9) | 12 + 4 × 4.6 + 24.2 = **54** / 12 + 18 + 13.9 = **44** | 12 + max(18, 29) = **41** / 12 + max(18, 18.5) = **31** |
| 4 × 10¹⁰ | 68.0 (exposed 40.7) | 87.3 | 46.0 (exp 29.2) / 33.7 (exp 16.8) | 9 + 4 × 7.7 + 29.2 = **69** / 9 + 31 + 16.8 = **57** | 9 + max(31, 37) = **46** / 9 + max(31, 25) = **40** |
| 8 × 10¹⁰ | 81.3 (exposed 53.3) | 327 (drifted; 216 with the write) | 51.0 (exp 33.8) / 36.8 (exp 19.5) | | |
| 10 × 10¹⁰ | 84.1 (exposed 56.3) | — | 51.6 (exp 34.5) / 37.1 (exp 20.1) | | |
| 576 × 9.16 × 10¹⁰ (the target, write 0.6) | 1933 (exposed 1681, 87 %) | — | 2304 PEs: 728 (exp 667) / 467 (exp 406) | 12 + 4 × 48 + 667 = **871** / 12 + 192 + 406 = **610** | **≈ 720 / 470** |

Reading: the model's per-PE compute at D/4 is ≈ a quarter of the node's (plus init, which the four processes do concurrently), so
the node's compute stays; the fabric term shrinks with the per-NIC bytes (each PE moves a quarter of the node's bytes through its own
NIC). At the target the fabric is 87 % of the wall and the form is worth **2.2–4 × modelled** (1933 → 470–870 s); at 4 nodes the
modelled gain is 68 → 40–69 s against 87 measured — the prototype has to show the measured 1-PE rate (2.8–3.6 GB/s per thread) turn
into ≥ 8 GB/s per PE while the compute is not slowed by the time-sharing. The `--bw` values are **assumed** from C16's 7.6–8.2 GB/s
per PE at one context (= 2 per thread) and the 14 GB/s one NIC gives four contexts (= 3.5 per thread); §2 measures the real ones.

##### 1.4 The intra-node exchange (question 2)

Cray's on-node transport is XPMEM (host memory; `SHMEM_SMP_SIZE_LIMIT` = the largest on-node copy, larger puts through the NIC
loopback — `man intro_shmem` "SMP Communication Layer"); ecalc's staging pool on Cray is the registered host pool
(`COMM_SHMEM_DEVHEAP=1` falls back to it, A16), so the on-node puts are host-to-host copies. The 909 GB/s xGMI push stays inside each
process (the four devices of one PE). What the 4-PE form adds is the *inter* stage's on-node share: 3 of 4n − 1 destinations
(43 % at 2 nodes, 8 % at 10, 0.1 % at 576) — measured in §2.2 (`t_comm --bw` with 4 PEs on one node). The hybrids of the task's
question 2 (helper PEs forwarding the fabric layer through shared memory / `hipIpc`) are only needed if §2 shows the time-sharing or
the on-node transport losing more than the four NICs gain.

##### 1.5 The tree (question 3)

g = 4n: `MN_GROUPS` default = the powers of two dividing 4n then the odd part (8 nodes → 2, 4, 8, 16, 32; 10 nodes → 2, 4, 8, 40);
levels 1–2 pair PEs of one node (on-node transport), level 3 up crosses nodes. `MN_PLAN_ONLY=<digits>:<4n>` (on aac7) gives the pool
per PE. At 576 nodes: 2304 PEs, `MN_GROUPS` 2, 4, …, 256, 768, 2304 (the odd part 9 = 3 × 3); the plan's pieces per level
and the critical path from `estimate.py --g 2304 --D 2.29e10` (the table above: memory per PE 226 device + 41 host; **4 × = 1068 GB
per node — does not fit**: the planes 104 GB per process are sized by the plane cap, not by D — the 4-PE form at the target needs the
plane pool bounded per process (`RNS_POOL_LOG` / `ECALC_NP`) — an open item for the user, §3).

#### 2. Measurements (aac7, ROCm 7.2.4 build of `~/ntt-P` at f31e3af = main 5a28bd3 + this branch; Cray OpenSHMEMX 11.8.0)

Branch history: `main` 5a28bd3 merged in (f31e3af) for S16's `COMM_INIT_EARLY=1` (the Cray `shmem_init_thread` segfault, 0/55 vs 8/78
launches) and R16's `BS_POOL_RULE`. New switch (off by default): **`MNRUN_NODES=<n>`** in `mnrun.sh` — at most the first n nodes of the
allocation (4 PEs on one node = `MNRUN_NODES=1 mnrun.sh 4`; a 2-node A/B inside the 4-node job). README row added. Nothing in the engine changed.

**Jobs and scripts** (aac7 `~/p16/P/`; times Eastern): **12208** (4 nodes, 16:49–17:02 EDT, `r1.sh`, logs `r1/`) — invalid as an A/B:
the aac7 clone had been pulled before `MNRUN_NODES` was committed, so "4 PEs on 1 node" ran 1 PE per node on 4 nodes and "8 PEs on 2 nodes"
ran 2 PEs per node on 4 nodes, and the 4-PE ecalc runs had `ECALC_CHECKPOINT=1` (trap below); then my second script (`r2.sh`) started steps
while r1's last ecalc step was still running (I had stopped r1's driver, not its step) → `Error configuring interconnect` on the overlapping
launch, r2 cancelled the job. The four nodes went through the cluster's routine post-job reboot ("reboot timed out" in `sinfo` for ≈ 10 min,
then back; every aac7 node reboots after a job — BootTime of the idle nodes = the end of their last job). **12211** (4 nodes
aac7-n31, s4b0n0, s4b1n0, s5b0n0; 17:11–17:25 EDT, `r3.sh`, logs `~/p16/P/r3/`, strictly sequential): **the valid set below**.
Common ecalc environment: `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 COMM_INIT_EARLY=1 ECALC_CHECKPOINT=0 ECALC_VERBOSE=2
COMM_LAYER_STATS=1`, 4-PE arm + `ECALC_PLANE_CAP=30`; launch `MNRUN_NODES=<n> ./mnrun.sh <PEs> env … ./ecalc 10000000000 /tmp/P/<tag>.out`
(`MNRUN_CPUS_PER_TASK=auto`: 48 CPUs per PE at 4 per node); the parts copied to NFS after each run (outside the timing) and compared by
`~/p16/B/pcmp.sh` against the first 10¹⁰ + 2 bytes of `~/ntt/ecalc/results/e_1e11.out`.

##### 2.1 (a) `t_comm --bw` — the on-node transport (Cray XPMEM / NIC loopback) — **measured**

`MNRUN_NODES=<n> MNRUN_PLAN_POOL=0 ./mnrun.sh <PEs> env COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_INIT_EARLY=1 COMM_SHMEM_POOL_MB=<pool>
./tests/t_comm --bw <threads> 20 <max MiB>` (pool-resident buffers; PE 0's median GB/s received per thread):

| form | 64 KiB | 256 KiB | 1 MiB | 4 MiB | 16 MiB | 64 MiB | 256 / 128 MiB | log |
|---|---|---|---|---|---|---|---|---|
| **4 PEs on one node, 1 context each** (all 3 peers on-node) | 5.0 | 9.1 | 11.5 | **12.3** | 10.9 | 10.2 | **10.3** | `r3/tc_on1_t1_r1` |
| same, PEs 1–3 at 4 MiB / 256 MiB | | | | 12.3–12.4 | | | 8.0–8.3 (r2, an ecalc step ran on the node) | `r2/tc_on1_t1_r1` |
| 4 PEs on one node, 4 contexts each (ecalc's form) | | | | 3.7 per thread = **14.5–14.8 per PE** | | | (r2, ecalc on the node; the clean r3 run refused: 4 × 16.9 GB hugepage heap) | `r2/tc_on1_t4_r1` |
| **8 PEs on 2 nodes (4 per node), 1 context** (7 peers, 3 on-node) | 4.9 | 7.7 | 9.0 | 6.5 | 4.7 | 4.4 | 4.8 (aggregate col. 7.3) | `r3/tc_n2_4pe_t1_r1` |
| 2 PEs on 2 nodes (1 per node), 4 contexts — the C16 baseline, same day | 1.8 (5.1 agg.) | 3.7 (14.0) | 4.6 (17.9) | 4.9 (**19.4**) | 3.8 (15.1) | 3.3 (13.3) | 3.2 (**13.0**) | `r1/tc_n2_1pe_t4_r1` |

- **The on-node transport moves 10–12 GB/s per PE with one context** (≈ 40–49 GB/s summed over the node's four PEs) — 1.3–1.5 × one NIC's
  7.6–8.2 (C16), and **60–90 × slower than the in-process xGMI push** (909 GB/s). It is not a free lane: the on-node share of the 4-PE form's
  exchanges is a SHMEM copy, not an xGMI one.
- With 4 PEs per node on 2 nodes and one context each, **each PE gets 4.4–9.0 GB/s (median), the node 7.3 × 4 ≈ 29 GB/s** at 128 MiB slabs vs
  13–19 GB/s for the one-PE node with four contexts — the four-NIC gain C16 predicted (31–32), now with on-node traffic in the mix.
- Not measured cleanly: 4 contexts per PE at 4 PEs per node (the pool 16 GiB per PE is refused by the hugepage heap; 8 GiB ran only under
  load in r2). `SHMEM_SMP_SIZE_LIMIT=1 GiB` ran in r1 only in the wrong placement (1 PE per node) — no reading.

##### 2.2 (b) ecalc 10¹⁰: 1 PE vs 4 PEs per node (job 12211, interleaved, ROCm 7.2.4) — **measured**

**All 11 runs of 12211 are byte-identical to the reference** (`pcmp.sh`: "identical (first 10000000002 bytes = the reference, length 10000000003)", `~/p16/P/r3.log`); the copies were removed afterwards.

| nodes | form | PEs | `total` s | bs | dm | init | total − init | all-to-all per PE (GB received, GB/s per APU thread) | digits |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 PE | 1 | 33.08 | 10.1 | 6.0 | 13.8 | 19.3 (incl. 10dP 0.9, other 2.2) | — | identical |
| 1 | **4 PEs** | 4 | **60.58** | 8.8 | **36.6** | 15.0 | 45.6 | 149–161 GB, **2.35** (all on-node) | identical |
| 2 | 1 PE r1 / r2 | 2 | 38.13 / 39.05 | 5.8 / 5.7 | 13.7 / 13.6 | 18.4 / 19.5 | 19.7 / 19.5 | 181–187 GB, 3.41–3.44 | identical / identical |
| 2 | **4 PEs** r1 / r2 | 8 | **31.32 / 36.53** | 7.5 / 7.7 | 12.9 / 16.6 | 10.8 / 12.1 | 20.5 / 24.4 | 108 GB, 1.77–1.81 | identical / identical |
| 4 | 1 PE r1 / r2 | 4 | 44.64 / 35.37 | 6.3 / 6.3 | 12.8 / 12.9 | 25.4 / 16.1 | 19.2 / 19.3 | 149–156 GB, 1.93–2.81 | identical / identical |
| 4 | **4 PEs** r1 / r2 | 16 | **25.87 / 25.96** | 5.4 / 5.3 | **8.1 / 9.4** | 12.3 / 11.2 | **13.5 / 14.8** | 66 GB, 1.42–1.53 | identical / identical |
| 4 | 2 PEs (control) | 8 | 28.86 | 6.8 | 11.1 | 10.9 | 18.0 | — | identical |

Reading (**measured**, 1–2 runs per cell — the run-to-run spread of `init` is 16–25 s in the 1-PE arm):
- **At 4 nodes the 4-PE form is faster: 25.9–26.0 s vs 35.4–44.6 s; the compute after init 13.5–14.8 vs 19.2–19.3 s (−25 to −30 %); `dm`
  8.1–9.4 vs 12.8–12.9.** The node moves ≈ 4 × 66 = 264 GB at ≈ 23 GB/s (4 PEs × 4 threads × 1.45) against 150 GB at ≈ 8–11 GB/s
  for the one-PE node: more bytes (the inter stage now spans 16 PEs) but 2–3 × the node's fabric rate — the four NICs.
- **At 2 nodes it is a wash**: compute 20.5 / 24.4 vs 19.7 / 19.5 s; the wall is shorter (31.3 / 36.5 vs 38.1 / 39.1) only because `init` is
  shorter (10.8–12.1 vs 18.4–19.5). 3 of each PE's 7 peers are on the node (43 %, §1.4) and the on-node lane is 10–12 GB/s, not xGMI.
- **On one node it loses 2 ×** (60.6 vs 33.1; `dm` 36.6 vs 6.0): the in-process xGMI exchange replaced by SHMEM on the node — the bound the
  form pays where the on-node share is large; it shrinks as 3 / (4n − 1) with the node count.
- **The time-sharing of the APUs costs little in bs**: 4 processes on each APU at D/4 each, bs 5.3–7.7 s vs 5.7–6.3 (single-node bs 8.8 vs 10.1).
- **`init` is shorter in the 4-PE form** (10.8–12.3 vs 16.1–25.4 s) — the per-process init work is a quarter and the four processes do it
  concurrently; the handshake (C16: ≈ 10 s of the 1-PE init) is not longer with 4 × the PEs at these counts.
- **The 2-PE control at 4 nodes** (8 PEs) = 28.9 s, compute 18.0 — between the two.
- Against the §1.3 model (2 nodes: optimistic 31–41, pessimistic 44–54; 4 nodes: 40–46 / 57–69 s modelled for the no-write wall) the
  measured 4-PE walls 31–37 (2 nodes) and **26 (4 nodes)** are at or below the optimistic end; the 1-PE arm's measured 35–45 at 4 nodes is
  also below the model's 68 (the model's 1-PE figure is the C16 profile's — **modelled** and measured are not the same quantity here: the
  model counts the write at 100 GB/s and has a larger exposed share at 4 nodes than measured).

**Trap found (r1): `ECALC_CHECKPOINT=1` with several PEs per node fails T2.** The budgeted top-level checkpoint (`ECALC_CKPT_TOP=2`) of every
PE goes to `<outfile>.top` (`mn_out.c mn_out_ckpt_default`) — **one path per node**, so the PEs of a node overwrite each other's file and
read it back: in r1's 8-PE run (2 PEs per node) PEs 4–7 reported `digits == X mod q FAILED` / `VERIFY FAILED` in both runs (`r1/e_n2_4pe_r*.log`);
PEs 0–3 passed. With `ECALC_CHECKPOINT=0` every run is identical. One r1 16-PE run with `ECALC_CHECKPOINT=1` took **238 s** (bs 42.6, dm 140.6,
fabric 0.11–0.39 GB/s per thread; VERIFY OK, digits not compared) — not explained; the same configuration with the checkpoint off is 26 s (§3 item 3).

##### 2.4 Follow-up (the coordinator's request): 8 nodes at 10¹⁰ and 4 nodes at 4·10¹⁰ — **measured**

Job **12264** (8 nodes aac7-n20, s3b0n0, s3b1n0, s4b0n0, s4b1n0, s5b0n0, s5b1n0, s6b0n0; 09:48–10:29 EDT 2026-10-05; `~/p16/P/r4n.sh`,
logs `~/p16/P/r4/`; clone `~/ntt-P` at b099950, ROCm 7.2.4). It was submitted when the coordinator gave the go-ahead (D16E 12262 cannot
start before ≈ 17:30 EDT). Same environment as §2.2 (`ECALC_CHECKPOINT=0 COMM_INIT_EARLY=1`, the 4-PE arm `ECALC_PLANE_CAP=30`). Digits were
compared with `pcmp.sh` against `e_1e11.out`.

| run | PEs | `total` s | bs | dm | init | total − init | all-to-all (GB/s per APU thread) | digits |
|---|---|---|---|---|---|---|---|---|
| 8 nodes 10¹⁰, 1 PE r1 / r2 | 8 | 42.21 / 33.05 | 6.0 / 6.1 | 17.1 / 10.4 | 19.1 / 16.6 | 23.1 / 16.5 | 1.86 / 2.16 | identical / identical |
| 8 nodes 10¹⁰, **4 PEs** r1 / r2 | 32 | **stopped at init** | | | | | | none written |
| 4 nodes 4·10¹⁰, 1 PE r1 / r2 | 4 | 223.22 / **93.85** | 51.7 / 24.0 | 102.2 / 43.4 | 69.3 / 26.4 | 153.9 / 67.4 | 1.19 / 2.87 | identical / identical |
| 4 nodes 4·10¹⁰, **4 PEs** r1 / r2 | 16 | **timeout 1500 s** / 228.49 | — / 66.8 | — / 121.4 | (≥ 93 per PE in r1) / 40.1 | — / 188.4 | — / 0.44 | none / **identical** |

- **A bug found at 32 PEs (on main, in both forms): every launch of 32 or more node-processes stops at init.** `mn_selftest_layered(11, 11)`
  (`ecalc.c` → `mn.c`) spreads 2¹¹ rows over 4 × gt ranks (gt = the largest power of two ≤ the PEs) and refuses below 32 rows per rank:
  `mn_selftest_layered: R / ranks < 32` → `VERIFY FAILED`, rc 1 (`r4/e_n8_4pe_r*.log`). At 32 PEs, R / ranks = 2048 / 128 = 16. It affects
  any run with g ≥ 32, so **the 576-node target (gt 512) would stop the same way** (an inference from the code; not run). New switch
  **`MN_SELFTEST_GROW=1`** (f90c21c, off by default; README row) raises logR until R / ranks ≥ 32. At gt 512 that is 2¹⁶ × 2¹¹ points (1 GiB
  per host array per APU thread) and C / ranks = 1 (**assumed** acceptable to `dist_plan`). The rerun with it is §2.5.
- **4·10¹⁰ is noisy in both arms**: the 1-PE arm took 223 s and then 94 s (B16 measured 87 s). In the slow 1-PE run, recip's single-node
  chain took 30.4 s instead of 2.3 s, and the fabric ran at 1.19 instead of 2.87 GB/s per thread. That run came right after the two
  failed 32-PE launches. **The 4-PE arm is 2.4× slower than the fast 1-PE run**: 228 s, recip chain 33.5 s, fabric 0.44 GB/s per thread.
  Its first run spent 343 s in the single-node chain and timed out at 1500 s; the step was killed by `timeout`, and the next launch on
  the same job worked.
  - At this size the four processes per node sit at 432 GB modelled per node (§2.3), and the nodes reported MemAvailable ≈ 122 GB with
    73 GB cached (tmpfs outputs) during the tree.
  - Memory pressure is the likely cause, but **not proven**: no `vmstat` / reclaim counters were taken.
  - The replicated single-node recip chain (each PE runs it on the same four APUs) is the other suspect (33 s vs 2.3 s).

##### 2.5 8 nodes, 32 PEs with `MN_SELFTEST_GROW=1` — **measured**

Job **12272** (8 nodes aac7-n11, s3b0n0, s3b1n0, s4b0n0, s4b1n0, s5b0n0, s5b1n0, s6b0n0; 11:28–11:33 EDT; `~/p16/P/r5b.sh`, logs
`~/p16/P/r5/`; clone `~/ntt-P2` at f90c21c). It started only after 12264's runs had ended and no P16 job was left. Same environment plus
`MN_SELFTEST_GROW=1` in both arms; the switch does nothing at 8 PEs (2¹¹ / 32 = 64 rows per rank).

| run | PEs | `total` s | bs | dm (recip) | init | total − init | single-node recip chain | all-to-all GB/s per APU thread | digits |
|---|---|---|---|---|---|---|---|---|---|
| 4 PEs r1 | 32 | 121.59 | 8.3 | 75.8 (60.9) | 37.4 | 84.1 | **53.75 s** | **0.14** | identical |
| 1 PE r3 | 8 | 30.79 | 6.0 | 10.6 (5.3) | 14.2 | 16.6 | 2.37 s | 2.15 | identical |
| 4 PEs r2 | 32 | 113.80 | 17.2 | 60.2 (50.1) | 36.3 | 77.4 | **24.76 s** | **0.12** | identical |

- **The switch works.** The layered self-test runs at 2²³ points (32 × 4 ranks, 13–14 s instead of 0.24 s at 8 PEs) and passes. All three
  runs are byte-identical to the reference.
- **At 8 nodes the 4-PE form is 3.7× slower than 1 PE per node**: 114–122 s against 31–42 s (three 1-PE runs: 42.2, 33.1, 30.8).
  This reverses the 4-node result (§2.2: 26 vs 35–45 s). Two things go wrong at 32 PEs:
  - **The replicated single-node recip chain takes 25–54 s** (2.4–3.1 s at 4–16 PEs in either form, §2.2 logs).
  - **The fabric drops to 0.12–0.14 GB/s per APU thread** (1.4–1.8 at 16 PEs).
  - The same pathology appears in the 4·10¹⁰ 4-PE runs on 16 PEs (§2.4: chain 33–343 s, 0.44 GB/s). So it is not the PE count alone.
  - Cause **not found**. Suspects:
    - the progress threads of 4 SHMEM processes × 4 contexts per node competing for the CPU (`MNRUN_CPUS_PER_TASK=auto`, 48 CPUs per PE);
    - Cray's per-PE resources at 32 PEs × 4 contexts (128 endpoints);
    - the four copies of the single-node chain on the same APUs, if that chain synchronizes with the other PEs per step.
- `init` doubles (36–37 vs 14–19 s): the self-test at 2²³ points accounts for 13–14 s of it; the rest is the 4 × larger handshake.

##### 2.3 (c) `MN_PLAN_ONLY` memory per PE (login node `aac7-login`, 2026-10-04 16:37–16:45 EDT) — **modelled** (the plan's own node figure)

`env COMM_TRANSPORT=shmem [ECALC_PLANE_CAP=30] MN_PLAN_ONLY=<digits>:<g> ./ecalc` — `plan cache … node <GB>` (plane pools + arena + tables +
the init host, the per-process figure the run's budget check uses against `ECALC_NODE_GB` = 480) and `plan pool` (the SHMEM pool per process):

| digits : g (PEs) | form | plane cap | pools per APU (GiB) | **node figure per process** | × processes per node | SHMEM pool per PE |
|---|---|---|---|---|---|---|
| 10¹⁰ : 2 | 1 PE/node, 2 nodes | 2³¹ (default) | 12 + 12 | 170.4 GB | 170.4 | 8704 MiB |
| 10¹⁰ : 4 | 1 PE/node, 4 nodes | 2³¹ | 12 + 12 | 151.1 GB | 151.1 | 4608 MiB |
| 10¹⁰ : 8 | 4 PEs/node, 2 nodes | 2³¹ | 12 + 12 | 142.5 GB | **570 GB — over the node (501 GiB = 538 GB)** | 2560 MiB |
| 10¹⁰ : 8 | 4 PEs/node, 2 nodes | **2³⁰** | 6 + 8 | **99.5 GB** | **398 GB** | 2560 MiB |
| 10¹⁰ : 8 | 4 PEs/node | 2²⁹ | 3 + 4 | 99.5 GB | 398 GB (no gain below 2³⁰ here) | 2560 MiB |
| 10¹⁰ : 16 | 4 PEs/node, 4 nodes | 2³⁰ | 6 + 8 | 90.9 GB | 364 GB | 1536 MiB |
| 4·10¹⁰ : 4 | 1 PE/node, 4 nodes | 2³¹ | 12 + 12 | 206.9 GB | 206.9 | 8960 MiB |
| 4·10¹⁰ : 16 | 4 PEs/node, 4 nodes | 2³¹ | 12 + 12 | 151.1 GB | 604 GB — over | 4608 MiB |
| 4·10¹⁰ : 16 | 4 PEs/node, 4 nodes | 2³⁰ | 6 + 8 | 108.1 GB | **432 GB** | 4608 MiB |

- **The 4-PE form does not fit at the default plane cap** (4 × 142.5 = 570 GB at 10¹⁰ on 2 nodes): each process sizes its plane pools for
  the cap, not for its D/4. With `ECALC_PLANE_CAP=30` (an existing switch: `POOL_LOG=30 DIST_LOGN_TEST=30`) it fits: 398 GB (2 nodes) and
  364 GB (4 nodes) at 10¹⁰, 432 GB at 4·10¹⁰ on 4 nodes. The floor below 2³⁰ (≈ 85 GB per process at 10¹⁰) is the arena, the tables and
  the init host, not the planes. The plan's piece counts are the same at 2³⁰ (the largest transform at 10¹⁰ is 2³⁰).
- **At the target size (5.276·10¹³ digits, 576 nodes)**: 1 PE/node (g 576, cap 2³¹) = **417.4 GB** per node (the transform cache gets
  0 slots: room 30.7 GB); 4 PEs/node (g 2304) = 271.3 GB per process at 2³¹ (no mn grid product: no cache), **254.1 GB at 2³⁰ of which
  51.5 GB is the 2-slot cache → 202.6 GB without it**, the same 271.3 at 3·2²⁹. **× 4 = 810–1085 GB per node: the 4-PE form does not fit at
  the target with any existing switch** (the target node: 512 GB class). Pieces: 220 at g 576 vs 78 (2³¹) / 102 (2³⁰) at g 2304. Splitting
  the figure (**modelled**, from these plan lines): planes 103 GB (2³¹) / 60 GB (2³⁰) per process; the rest is 314 GB at D/576 and
  143 GB at D/2304, which fits **≈ 86 GB fixed per process + 228 GB × (per-PE digits / (D/576))** (the 10¹⁰ floor of ≈ 85 GB above agrees).
  So four processes carry 4 × (86 + planes) ≈ 580 GB of fixed cost before any digit: quartering the per-PE digits does not quarter the
  node figure (§3 item 2).
- Each process checks its own figure against `ECALC_NODE_GB` (480) and knows nothing of the other three: the 4-PE form needs
  `ECALC_NODE_GB` ≈ 480 / 4 per process to make the existing budget check true (§3 item 2).

#### 3. Open items and A/B options for the user

**What the measurements say.** The 4-PE-per-node prototype needs no engine change, keeps the digits identical, and at 4 nodes cuts the
10¹⁰ compute after init by 25–30 % (13.5–14.8 vs 19.2–19.3 s) and the wall to 26 s (vs 35–45) — **measured**, two runs per arm. At 2 nodes
it is even on compute; on one node it is 2 × slower. The gain grows with the node count as the on-node share of the inter stage
(3 / (4n − 1)) shrinks and the four NICs carry the bytes — the direction §1.3 modelled. **But it does not fit the target's memory** (§2.3:
4 × 203–271 GB per node at 5.276·10¹³ digits on 2304 PEs).

1. **A/B for adoption at test scale (nothing to switch on in the code: a launch form).** `MNRUN_NODES` (new, off) only selects nodes. The
   form is `mnrun.sh 4n … ECALC_PLANE_CAP=30 ECALC_CHECKPOINT=0`. Options: (A) keep 1 PE per node (the default); (B) adopt 4 PEs per node
   for aac7 multi-node runs ≥ 4 nodes with `ECALC_PLANE_CAP=30`; (C) first repeat at 8 nodes and at 4·10¹⁰ / 10¹¹ on 4 nodes (not run here:
   the job budget; B16's 4·10¹⁰ on 4 nodes is 87 s measured, the plan says 432 GB per node at 2³⁰ — tight). **Recommendation: (C) before (B)**
   — two runs per cell at one size is too little to adopt on, and the 1-PE arm's `init` spread (16–25 s) is larger than the gain at 2 nodes.
2. **Memory per process (blocks the target).** Each process sizes its plane pools by the plane cap and carries a fixed ≈ 86 GB (arena,
   tables, init host — **modelled** from the plan lines); the budget check (`ECALC_NODE_GB` 480) is per process and knows nothing of the
   other three. Options: (A) a per-node budget switch (`ECALC_NODE_GB` divided by the PEs on the node — needs the node-local rank count;
   small); (B) shrink the fixed part per process (which tables / arenas are duplicated four times on a node — a study of `mem_model.py`'s
   node figure split by kind); (C) the real "one PE = one APU" form of §1.1 (prime d on device d mod 4 of PE d; weeks) — each process then
   holds one APU's share, the node figure ≈ the 1-PE figure (**assumed**, not planned), and the on-node exchange would need `hipIpc`. Without (B) or (C) the 4-PE
   form is a test-scale tool only.
3. **`ECALC_CHECKPOINT=1` with several PEs per node** — the top checkpoint path `<outfile>.top` is shared by a node's PEs: T2 fails (r1).
   Fix options: (A) name it `<outfile>.top<rank>` when `COMM_SIZE` > the node count (behind a switch, e.g. `MN_CKPT_PER_PE=1`); (B) document
   `ECALC_CHECKPOINT=0` as required for the form. The one 238 s 16-PE run with the checkpoint on is unexplained (one run, fabric 0.1–0.4 GB/s
   per thread) — a repeat with (A) would show whether the shared file or the tmpfs writes (4 × 1.1 GB per node) caused it.
4. **The on-node lane is 10–12 GB/s per PE, not xGMI** (§2.1). The hybrids of §1.4 (a helper PE per node forwarding the fabric layer, or
   the on-node stage by `hipIpc`) would remove the 2-node / 1-node penalty; worth it only if the target form is several PEs per node
   (item 2). The 4-context-per-PE on-node rate was not measured cleanly (the hugepage heap refused 4 × 16.9 GB) — rerun with
   `COMM_SHMEM_POOL_MB=8192 … --bw 4 20 32` on one node if needed.
5. **The model** (§1.3) under-predicts both arms at 2–4 nodes (1-PE: 68 modelled vs 35–45 measured at 4 nodes): the C16 profile's exposed
   fabric at 4 nodes is too high, or the write term (100 GB/s) — F16's model owner should take the 4-node 1-PE / 4-PE pairs as calibration points.
6. **Not done**: 8 nodes (≤ 4 nodes allowed here); 4·10¹⁰ in the 4-PE form; `SHMEM_SMP_SIZE_LIMIT` sweeps; the hang watcher of C16's
   scripts (no hang occurred in 12211).

##### 3.1 After the follow-up (§2.4–2.5): revised open items

7. **`MN_SELFTEST_GROW` should be on for any run with 32 or more node-processes, the target included.** Without it the launch stops at init,
   in either form. Options: (A) make it the default (the self-test only grows where it now refuses; at gt 512 it costs 2¹⁶ × 2¹¹ points
   per APU thread, ≈ 1 GiB × 4 host arrays and a few seconds **modelled**); (B) cap the self-test at gt ≤ 16 ranks (test a subgroup);
   (C) leave it off and set it in the target's launch line. **Recommendation: (A) or (B) before any ≥ 32-PE run.** The 1-PE form at the target
   (576 PEs) hits this exactly as the 4-PE form does; this conclusion comes from the code, not from a run.
8. **The 4-PE form is not ready beyond 16 PEs.** It wins at 4 nodes / 10¹⁰ (16 PEs, §2.2). It loses 3.7× at 8 nodes / 10¹⁰ (32 PEs) and
   2.4× at 4 nodes / 4·10¹⁰ (16 PEs, 432 GB per node modelled). Both losses carry the same signature: a slow single-node recip chain and
   fabric below 0.5 GB/s per thread. Option (A): diagnose it (one 32-PE run with `COMM_LAYER_STATS` per step, `vmstat 1` and `top -H`
   on a node, and the chain run once per node instead of per PE). Option (B): drop the form and spend the effort on the per-node budget and
   one-PE-per-APU (§3 items 2, 4). **Recommendation: (B) unless the target's SHMEM is known to require one PE per NIC.** The 4-node gain
   (−25 to −30 % compute) does not survive the scale-up at the sizes measured.
9. **4·10¹⁰ on 4 nodes is noisy even in the 1-PE arm** (223 vs 94 s; B16: 87 s): a pair of runs per cell is too few at this size on aac7.

#### RESUME
- 2026-10-05 11:45 EDT: **follow-up done.** Jobs 12264 (r4n, 8 nodes, 09:48–10:29 EDT) and 12272 (r5b, 8 nodes, 11:28–11:33 EDT) were
  both cancelled by their scripts; no P16 job is left; `~/p16/P/out` is empty. §2.4–2.5 and §3.1 written. New switch `MN_SELFTEST_GROW` (f90c21c);
  clone `~/ntt-P2` holds it. D16E 12262 was still pending and untouched. Branch p16-P pushed, not merged.
- 2026-10-05 08:50 EDT: r4 restarted (pid 1544704) with a stricter gate (the coordinator: D runs one more 10-node D16E job via
  `~/p16/D/e82.sh` after s8): opens only when `s8c/DONE` exists, no D16 / D16E job is queued and no `e82.sh` driver runs, checked twice
  5 min apart (every 30 min otherwise). Nothing submitted yet.
- 2026-10-04 20:52 EDT: follow-up armed (the coordinator's request): `~/ntt-P` pulled to b099950 and rebuilt (7.2.4, `build2.log`);
  `~/p16/P/r4.sh` (pid 1256262, log `r4.log`, runs `r4/`) checks every 30 min for `~/p16/D/s8c/DONE` with no D16 / D16E job left, then
  submits one 8-node P16 job (1:15) and runs, strictly sequentially: 10¹⁰ on 8 nodes 1 PE / 4 PEs ×2 interleaved, then 4·10¹⁰ on 4 nodes
  (`MNRUN_NODES=4`) ×2; 4-PE arm `ECALC_PLANE_CAP=30`, all `ECALC_CHECKPOINT=0 COMM_INIT_EARLY=1`; then cancels the job and runs pcmp → `R4_DONE`.
  Plan (modelled): 10¹⁰ on 32 PEs = 90.9 GB per process (364 per node). If this session dies: wait for `R4_DONE` in `r4.log`, fill §2.4.
- 2026-10-04 17:55 EDT: **§2 and §3 done.** Valid set = job 12211 (r3, 17:11–17:25 EDT, cancelled by the script; all 11 runs identical);
  12208 (r1/r2) invalid as an A/B (stale clone without `MNRUN_NODES`; overlapping steps) — kept for the checkpoint trap. No job of mine on
  aac7 (`squeue -u <user> | grep P16` empty); `~/p16/P/out` emptied. Branch p16-P pushed, not merged. Next (the user's choice, §3): repeat
  at 8 nodes / 4·10¹⁰, the per-PE checkpoint path, the per-node budget.
- 2026-10-04 16:50 EDT: resumed from af12caf (the previous agent's 2-node job 12207 had no script attached: cancelled). main merged
  (f31e3af, `COMM_INIT_EARLY`), `MNRUN_NODES` added (ba0ec70), `~/ntt-P` rebuilt with rocm/7.2.4 (`~/p16/P/build.log`). §2.3 done (login).
  **J = 12208** (4 nodes, 1:20, `P16`, pending behind B16 12193 / D16E 12195 — 10-node jobs; earliest start ≈ 22:50 EDT). `~/p16/P/r1.sh`
  (setsid, waits for 12208; log `~/p16/P/r1.log`, per-run logs `~/p16/P/r1/`, digits copied to `~/p16/P/out/<tag>/`) runs: t_comm on one
  node (4 PEs) and 8 PEs on 2 nodes; ecalc 10¹⁰ 1 PE vs 4 PEs (`ECALC_PLANE_CAP=30`) ×2 interleaved at 2 nodes + a 1-PE cap-30 control,
  the same ×2 at 4 nodes, then 4·10¹⁰ once each if ≥ 25 min remain; cancels 12208, then `pcmp` of every run → `R1_DONE`.
  Next: read `r1.log`, fill §2.1–2.2, §3; `rm -rf ~/p16/P/out` after the compares.

## C02 (opportunity C8): the gap between the layered exchange rate and the NIC rate -- where it goes at 4 / 6 nodes, three peer schedules, the plan

(former file: C02.md)

Design + microbenchmark plan, 2026-10-11 (wave C, agent c02). **No engine change** (comm_shmem.c / comm_ofi.c / comm_layered.c untouched; they are
c03's / c06's in wave C). New files: `ecalc/tests/t_a2a_sched.c` (stand-alone Cray SHMEM probe, test code only) and the aac7 queue item
`archive:drivers/c02/50_c02_sched4.sh` (installed as `~/aac7q/queue/50_c02_sched4.sh`, NODES=4, EST_MIN=12, dry-run STUB=1 locally and on the
login node). Labels: **(m)** measured, **(d)** derived by arithmetic from measured numbers, **(mod)** modelled, **(a)** assumed. Times Eastern.

Inputs (read-only, bounded greps): aac7 `~/s48/log/n{2,4,6}_r{1,2}.log` (S48: 6.441e10 digits per node, `COMM_XSTATS=1 MN_WAIT_STATS=1`, X2 on),
`~/sess1011b/t09/log/p*.log` (T09: 4 nodes, fast and slow mode), `~/sess1011c/a13/A/log/r00{4,6}.log` (A13: quiet cluster, slow r004 / fast r006);
results S19B, XEFF, S27, S31, T05, T06, T09, A07, A13; code `comm_shmem.c` (`push_peers`, `ready_first`, `rounder`, `put_signalled`, `ofi_flush`),
`comm_ofi.c` (`comm_ofi_write`: 4 MiB chunks, window 64 per NIC, `FI_DELIVERY_COMPLETE`), `comm_layered.c` (`y_alltoall`, `y_alltoallv2`, `v_post`),
`mn.c` (`mn_group_at`, `mn_group_span`, `mn_mesh2`), `mn_model.py` (`Fabric.a2a`).

#### 0. Answer in short

1. **At 4 nodes (power of two, `layeq`) the layered inter stage already runs at 18.2-19.1 GB/s per APU thread = 78-82 % of the NIC's 23.3 (m, fast
   mode).** The "10.1 vs 23.3" gap of XEFF is a 10-node, 9-peer, `layv` figure. At 4 nodes the schedule can win at most ~4 GB/s (≈ 2-7 s of
   wall, mod); the **slow launch mode** (A13: NIC MCU group A, ≈ 30 % of launches) costs more than that: the same stage runs at 12.6-14.2 GB/s
   (×0.69-0.74, m) and every exchange kind slows by the same factor.
2. **At 6 nodes (`layv`, general map, X2 on) the per-thread xstats rate of 7.0-8.6 GB/s (S48 / T06's "2.3× slower") is an accounting artefact of
   `COMM_LAYER_INTER2`:** two inter stages are in flight and `t_post` counts the overlap twice. From the post-to-post spans (Σ `t_call`) the
   `layv` stage moves its bytes at **13.4-15.6 GB/s** (d), i.e. 0.7-0.8 of the 4-node `layeq` rate, not 0.41. Of the time a thread is
   blocked in a `layv` wait (64-72 s per thread over the run), **roff = waiting for a peer that has not posted yet (skew) is 30-39 s**, the
   transport-shaped part (`sig` + staging/flush/copy-out) 33 s (m, S48 r1/r2).
3. **Candidate schedules.** Rotated start (X1, `rot`) is in the code and null on wall (S27). The next step up is **chunk-interleaved posting
   (`rr`: every sender feeds all its peers at once, no moving incast; `winK`: at most K peers open)**: zero memory, ≈ 50 lines in
   `comm_shmem.c` only, and -- the point that makes it worth testing now -- it is the only candidate that could also remove the slow-mode
   penalty if that penalty is receiver flow control under incast (A13: `oxe_stall_fgfc` ×2 in slow launches). Lockstep pairwise rounds (`pair`)
   and a two-level hierarchical all-to-all are modelled as worse or neutral at ≤ 10 nodes; the hierarchical one only pays at 576 nodes if the
   per-peer fixed cost is ≥ 8 µs (mod), which is unmeasured.
4. **Plan:** the stand-alone probe `t_a2a_sched` (queued, 4 nodes, ≈ 8 min of hold) measures seq / rot / rr / win2 / pair in one process per launch
   (paired within a launch, so the launch's MCU group is the same for every schedule) over 12 launches with NIC counters around each. Pass rule
   and the engine switch (`COMM_SHMEM_PEER_ORDER=rr|winK`, c03's file) are in §4.

#### 1. Where the gap goes today at 4 and 6 nodes (m unless marked)

Per PE, mean over the PEs of the run; seconds are per node summed over the 4 APU threads (as the xstats lines print them; ÷ 4 for per thread);
"rate" = GB received by the node / Σ post-to-completion seconds = GB/s per APU thread. `roff` = blocked on a peer's offset word (the peer has not
posted: skew), `sig` = blocked on signal words (data in flight / tail), `other` = `t_wait` − `roff` − `sig` (staging copies, posting, delivery-complete
flush, copy-out). `t_call` = post to the caller's wait (the caller's overlapped work).

##### 1.1 Four nodes, g = 4 (3 peers), `layeq` (equal-slab pipeline, one inter exchange in flight per mesh)

| run | mode | kind (phase) | n | GB | t_post | t_call | t_wait | roff | sig | other | rate GB/s/thr |
|---|---|---|---|---|---|---|---|---|---|---|---|
| S48 n4_r1 (182.0 s) | fast | layeq (dm) | 4640 | 1868 | 97.6 | 30.9 | 66.6 | 0.7 | 2.3 | 63.6 | **19.15** |
| | | layeq (bs) | 3744 | 1263 | 68.8 | 22.1 | 46.7 | 0.3 | 1.4 | 45.0 | **18.37** |
| | | layeq (recip) | 7464 | 657 | 38.0 | 13.3 | 24.6 | 0.6 | 1.1 | 23.0 | 17.32 |
| | | redist (dm) | 140 (548 rounds) | 209 | 27.8 | 0.1 | 27.7 | 3.7 | **14.5** | 9.5 | 7.54 |
| | | result (dm) | 272 | 175 | 23.3 | 0.1 | 23.2 | **7.2** | 3.8 | 12.3 | 7.51 |
| | | result (bs) | 232 | 149 | 16.8 | 0.1 | 16.7 | 4.8 | 3.1 | 8.8 | 8.89 |
| T09 p3_8 (209.0 s) | **slow** | layeq (dm) | 4640 | 1868 | 147.1 | 68.6 | 78.4 | 6.6 | 17.1 | 54.8 | **12.71** |
| | | layeq (bs) | 3744 | 1263 | 89.9 | 36.9 | 53.0 | 2.9 | 7.9 | 42.2 | 14.05 |
| | | redist (dm) / result (dm) | | 209 / 175 | 28.7 / 24.5 | | | 3.8 / 7.4 | 14.9 / 4.2 | | 7.29 / 7.15 |
| A13 r004 (220.0 s) | **slow** | layeq (dm) / (bs) | | 1868 / 1263 | 148.2 / 88.8 | 68.4 / 36.9 | 79.9 / 51.9 | 16.3 / 5.9 | 5.5 / 2.8 | 58.2 / 43.2 | **12.60 / 14.22** |
| A13 r006 (182.0 s) | fast | layeq (dm) / (bs) | | 1868 / 1263 | 97.9 / 69.4 | 29.0 / 21.8 | 68.9 / 47.6 | 0.8 / 0.5 | 2.6 / 1.5 | 65.5 / 45.5 | **19.08 / 18.20** |

Per APU thread, the inter stage alone (`xstats pe apu kind layeq` lines, n4_r1): 18.36 / 17.91 / 17.87 / 18.13 GB/s for APUs 0-3 (spread 2.7 %:
no slow mesh). The four fast 4-node runs (S48 r1/r2, T09 p2_2, p3_10, A13 r006) agree to 0.1 GB/s on every kind.

**Reading (4 nodes).**
- The layered inter stage is at **78-82 % of the NIC** in the fast mode. Per exchange (dm): 403 MB per APU thread in 21 ms post-to-completion
  (134 MB per peer); at the NIC's 23.3 GB/s it would be 17.3 ms, so the whole schedule + protocol + tail overhead is **≈ 4 ms per exchange** (d),
  below XEFF's ≈ 10 ms per-exchange estimate at 10 nodes (offset handshake, flush, the two stream syncs, the tail). The incast/order term is
  small here: with 134 MB per peer and a 256 MiB window per NIC (64 × 4 MiB), the NIC has ≤ 2 peers' chunks in flight anyway.
- **The slow mode is not a schedule effect at first sight but behaves like one:** every kind's rate drops by the same factor (layeq ×0.66-0.78,
  redist ×0.97, result ×0.95 -- the direct exchanges barely move because they are skew-bound), `sig` in layeq rises 2.3 → 17.1 s (p3_8) and
  `t_call` doubles (the caller waits longer at the intra all-gather of `vtab_build` / the xGMI stage for the slowest mesh). A13 saw ×2 fine-grained
  flow-control stalls (`oxe_stall_fgfc_*`) in slow launches: back-pressure at the receiver. **If that back-pressure comes from the sender-aligned
  order (every sender's queue starts with the same receiver), an interleaved schedule would change it; if it is a NIC-side MCU assignment, it would
  not.** The probe below separates the two (schedules compared inside one launch, MCU group logged per launch).
- The direct exchanges at 4 nodes are where XEFF put them: `redist` is 52 % `sig` (3.9 rounds per call under `COMM_SHMEM_ROUND_MB=1024`), `result`
  is 31 % `roff` (A07's volume gradient) + 53 % `other`.

##### 1.2 Six nodes, g = 6 (5 peers), `layv` (general map, X2 = INTER2 + VSLOT_POOL on, two inter stages in flight per mesh)

| run | kind (phase) | n | GB | t_post | t_call | t_wait | roff | sig | other | xstats "rate" | span rate (d) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| S48 n6_r1 (304.1 s) | layv (dm) | 8800 | 2625 | 353.8 | 197.4 | 156.4 | **77.0** | 46.1 | 33.3 | 7.42 | **13.3** |
| | layv (bs) | 4416 | 1317 | 189.3 | 105.0 | 84.3 | **50.7** | 22.5 | 11.1 | 6.96 | 12.5 |
| | layv (recip) | 6960 | 744 | 105.9 | 59.9 | 46.0 | 27.7 | 12.8 | 5.4 | 7.03 | 12.4 |
| | layeq (bs, level 2) | 1856 | 498 | 29.1 | 9.8 | 19.3 | 0.4 | 0.9 | 18.0 | **17.15** | 17.2 |
| | redist (dm) / result (dm) | 236 / 608 | 244 / 321 | 45.0 / 63.9 | | | 5.8 / **22.2** | **27.5** / 7.9 | 11.7 / 33.7 | 5.41 / 5.02 | |
| S48 n6_r2 (282.8 s) | layv (dm) | 8800 | 2625 | 303.9 | 167.4 | 136.5 | 50.5 | 23.8 | 62.2 | 8.64 | **15.7** |
| | layv (bs) | 4416 | 1317 | 160.0 | 87.8 | 72.2 | 34.3 | 12.0 | 25.9 | 8.23 | 15.0 |
| | layv (recip) | 6960 | 744 | 106.4 | 59.2 | 47.2 | 33.9 | 7.0 | 6.4 | 6.99 | 12.6 |
| | redist (dm) / result (dm) | | 244 / 321 | 48.7 / 60.9 | | | 8.6 / 21.6 | 28.4 / 7.2 | 11.6 / 31.8 | 5.00 / 5.28 | |

"span rate" (d): under INTER2 the caller posts exchange k, then completes k−1 (blocked `t_wait(k−1)`), then prepares k+1; so `t_call(k)` =
post(k) → post(k+1), the sum of `t_call` is the stage's post-to-post span, and GB / (Σ t_call / 4) is the rate at which the stage advances
per thread, with both communicators busy. It is a lower bound on the wire rate (the wire idles when k completes before k+1 is posted).

**Reading (6 nodes).**
- **T06's "layv 2.3× slower (0.41)" compares Σ `t_post` of a two-in-flight pipeline with Σ `t_post` of a one-in-flight one.** The span rate says
  `layv` at g = 6 advances at 0.73-0.86 of `layeq` at g = 4 (13.3-15.7 vs 18.2-19.1). T06's fitted 0.655 (one point) is in between. Hand to the
  model owner (a02): the `layv_factor` should be refit on span rates, or `COMM_XSTATS` should print the span (Σ `t_call` + the last wait) next
  to Σ `t_post` under INTER2 (S31 made the same remark).
- **The thread's blocked time in `layv` waits is 64-72 s per thread over the run (dm 34-39, bs 18-21, recip 12), and 30-39 s of it is `roff` = a
  peer has not posted its exchange yet.** That is skew between the 6 nodes inside the pipelined transform (the general map's 8 chunk exchanges per
  pass; every node must reach chunk k before anyone's chunk k completes), not the transport. The transport-shaped remainder (`sig` + `other`)
  is 33 s per thread. A faster wire can shrink only that part; the X1 lesson (S27: +20 % transport rate, 0 wall) is the roff part re-appearing
  elsewhere.
- Per exchange (dm): 298 MB per APU thread, 60 MB per peer, 40 ms post-to-completion (two in flight), `t_wait` 18 ms, `t_call` 22 ms. The window
  (256 MiB per NIC) holds all 5 peers' slabs at once, so the NIC's queue order **is** the schedule here: `seq` = 5 peers back to back.
- The direct exchanges grow with g: `result` (dm) 15.2-16 s per thread (A07's gradient, roff 5.4-5.6 s), `redist` (dm) 11-12 s (sig 7 s: 3.7
  rounds per call). Not this task's (C6 / RPIPE / REDIST_FUSE are a07 / c03).

##### 1.3 The gap, decomposed (per APU thread, per GB of inter-node bytes, fast mode; d / mod)

| step | 4 nodes `layeq` (134 MB per peer) | 6 nodes `layv` (60 MB per peer) | 10 nodes `layv` (41 MB per peer; XEFF / S27, X2 off) |
|---|---|---|---|
| raw NIC (NIC16, m) | 43 ms | 43 ms | 43 ms |
| achieved stage rate | 53 ms (18.7 GB/s, m) | 64-75 ms (13.3-15.7, d) | 88 ms (11.4, m); 73 ms with rot (13.7, m) |
| gap | **10 ms (19 %)** | **21-32 ms (33-43 %)** | **45 ms (51 %)**; 30 ms with rot |
| of which per-exchange fixed (handshake, flush, syncs, tail) | ≈ 4 ms per 403 MB exchange = **≈ 10 ms/GB: all of the 4-node gap** (d) | 4-10 ms per 298 MB exchange = 13-34 ms/GB (a: not separable from the order term with two in flight) | ≈ 10.5 ms per 745 MB exchange = 14 ms/GB (XEFF, mod) |
| of which peer order / incast / tail | ≈ 0-3 ms/GB | ≈ 0-15 ms/GB (a) | ≈ 15 ms/GB (rot: 11.4 → 13.7 is the measured order term) |
| slow launch mode (≈ 30 % of launches, A13 / T05) | +22-26 ms/GB (×0.69-0.74) | not measured at 6 n | T05: ×0.79 at 4 n |

**So the schedule term grows with the peer count (0 → 5-15 → 15 ms/GB from 3 → 5 → 9 peers) and the fixed per-exchange term is the floor at
small g.** Both are what §2's candidates address; the slow mode is the largest single term at 4 nodes and the probe tests whether a schedule
touches it.

#### 2. Candidate peer schedules (mod)

Model conventions. A mesh is one APU thread per node over g nodes (ecalc's layered inter communicator): one NIC per APU, 23.3 GB/s each way (m).
Per exchange a thread sends (g−1) slabs of s bytes. The sender's NIC drains its command queue in posting order with ≤ 64 chunks of 4 MiB in
flight (`comm_ofi_write`). The schedule is the posting order. Per-exchange fixed cost f ≈ 10 ms (XEFF, m at 10 n). Gains below are span
reductions converted to wall by the exposed fraction e: 4 n layeq e ≈ 0.7 (`t_wait`/`t_post`, m), 6 / 10 n layv with X2 e ≈ 0.5 (S31: exposed
layv wait 68.6 s of 1252 GB at ≈ 13.7 GB/s = 91 s span), and only the transport-shaped half of the exposed wait (the other half is roff skew, §1.2)
can shrink. Memory: the posting order needs no buffer; every candidate below is memory-neutral unless stated.

| schedule | what it does | 4 nodes | 6 nodes | 10 nodes | 576 nodes (a: the per-thread rate at 575 peers is not measured; XEFF §2's 2.07·S aac7-class / 0.26-0.44·S target) | memory | sync added |
|---|---|---|---|---|---|---|---|
| `seq` (today) | peers 0..g−1, a peer's whole slab before the next; all senders aligned on the same receiver (moving incast) | 18.7 GB/s (m) | 13.3-15.7 (d) | 11.4 (m) | every sender starts at receiver 0: 575 : 1 incast on the first receiver, then the next... (a) | 0 | 0 |
| `rot` (X1, in code, off) | rotated start me+1, ready-first | not measured at 4 n; model +0-3 % | +5-10 % (mod) | 13.7 (m, +20 %), **0 wall (m)** | removes the aligned incast (the measured order term) | 0 | 0 |
| **`rr` (A) chunk round robin over the ready peers, rotated start** | every receiver sees all its senders at once at 1/(g−1) of a stream each: no hot spot, every pair ends together (no tail), the window covers all peers | ceiling 19.5-20 (a: the fixed term stays): span 51 → 49 s, **−1.8 s wall**; ceiling at the NIC −7 s | 14.5 → 19.5 (a): span 85 → 63 s, transport-exposed 33 s × (1 − 14.5/19.5) = **−8 s** (ceiling −14 s) | 13.7 → 19.5 (a): span 91 → 64 s; transport-exposed ≈ 60 s × (1 − 13.7/19.5) = **−18 s** at most; X1 says the wall follows only with the skew fixed: quote **−5 … −18 s** | the window (64 ops) already bounds the open peers to 64 of 575 with 1.78 MB per pair (one chunk each): `rr` = `rot` there; gain = the incast term only (mod: −20…−40 s aac7-class, −3…−10 s target) | 0 | 0 |
| **`winK` (C) at most K peers open, chunk round robin among them, rotated start** | bounds the receiver's concurrent senders to ≈ K (K = 1: rot; K = g−1: rr); for 575 peers it caps the per-receiver stream count where the NIC's flow control would otherwise throttle | = rr for K ≥ 3 | K = 2-3: between rot and rr | K = 4: between | **the only knob at 576**: K = 8-32 (a) keeps each receiver at ≤ K streams of 1.78 MB instead of 64; gain unknown until measured at ≥ 64 peers (not possible on aac7) | 0 | 0 |
| `pair` (B) pairwise rounds in lockstep (round i: send to me+1+i, receive from me−1−i, complete, next) | one stream per NIC each way per round: no incast at all, but each round ends with its slowest pair and the next round's post waits | per-round tail (g−1 rounds × 1-2 ms sync + slowest pair per round, a): 3 rounds on a 21 ms exchange: **+15…+30 %** (mod) | 5 rounds on a ≈ 20 ms single exchange: **+25…+50 %** | 9 rounds on ≈ 30 ms: **+30…+60 %** | 575 rounds per exchange with 1.78 MB rounds = latency-bound (575 × 10 µs = 6 ms per 1 GB...): −, and the direct exchanges already run this way (`rounder`, K rounds, lockstep) | 0 | g−1 per exchange |
| hierarchical two-level all-to-all (24 × 24 at 576: exchange within a row, relay, exchange within a column) | 575 peers → 2 × 23 per exchange; bytes on the NIC ×2 (the relay) | − (bytes ×2 at 3 peers) | − | − | messages per thread 1.46 M → 0.12 M (mod, S19B); fixed term 2.5-4 k exchanges × 575 × (2 control words): at 2 µs −3…−5 s, at 8.5 µs **−12…−20 s**; bytes +13.7 TB per node = **+34 s at 100 GB/s per APU** (a) → net ≥ 0 unless the per-peer cost is ≥ 8 µs | + one relay buffer = the exchange's size (0.7-1.7 GB per APU at the largest v-slot) | 1 extra phase |
| intra-node-then-inter-node | | already the layered design (xGMI stage, then one NIC per APU; `comm_layered.c`) | | | | | |

**Best candidate: `rr` / `winK` as one switch value family** (`COMM_SHMEM_PEER_ORDER=rr` and `=winK`), because it is the only one that can (i) take
the measured order term at 10 nodes (rot's +20 %) further to the fixed-cost floor, (ii) possibly change the slow-mode incast back-pressure at
4 nodes, (iii) be bounded at 575 peers by K, and it costs no memory and no synchronisation. Its honest expected wall gain at ≤ 10 nodes is
**small (−2…−8 s at 4-6 nodes, −5…−18 s at 10 nodes, mod)** unless the slow mode responds, in which case it is −13 s on 30 % of 4-node launches
(≈ −4 s expected per launch, mod) and more at 10 nodes (the slow mode's size there is not measured).

#### 3. Microbenchmark (no engine change): `tests/t_a2a_sched.c` + queue item `50_c02_sched4.sh`

- **Program:** Cray OpenSHMEM, 4 PEs per node (one NIC per PE, as ecalc's 4 APU threads), meshes = PEs with the same local index over g nodes
  (`-g`), equal all-to-all of s MiB per peer in 4 MiB chunks posted with `shmem_putmem_signal_nbi(..., SHMEM_SIGNAL_ADD)` (the receiver's word
  of each sender counts delivered bytes; no fence), `shmem_quiet`, wait on g−1 words; schedules `seq`, `rot`, `rr`, `winK`, `pair` exactly as in §2;
  warm-up + barrier before each timed loop; median / min ms per exchange and GB/s per PE printed per PE; VERIFY of the data at the end.
  Host symmetric heap (`SHMEM_SYMMETRIC_SIZE=1500M`): the schedule effect is in the NIC and the fabric, not the memory kind (NIC16 #21: Cray SHMEM
  host heap reaches the same 23.2 GB/s per NIC).
- **Build (login node, done):** `cd ~/ntt-wt/c02/ecalc && cc -O2 -Wall -I$SMA/include -I$DS/include -o tests/t_a2a_sched tests/t_a2a_sched.c
  -L$SMA/lib64 -L$DS/lib -L/opt/cray/pe/lib64 -lsma -lpmi -lpmi2 -ldsmml -Wl,-rpath,...` with `module load cray-dsmml cray-openshmemx`
  (the Makefile's `SHMEM_CRAY=1` flags; `SMA=/opt/cray/pe/sma/11.8.0/ofi/sma`, `DS=/opt/cray/pe/dsmml/0.3.1/dsmml`). Compiles clean (m).
- **Item (installed 16:16 EDT, `~/aac7q/queue/50_c02_sched4.sh`, NODES=4, EST_MIN=12, APPROVED=coordinator-2026-10-11):** 12 launches; each
  launch runs all five schedules at 16 and 64 MiB per peer, 10 reps, g = 4 (odd launches) or g = 2 (even launches); NIC counters
  `oxe_mcu_meas_18..33` (A13's group A / B) and `oxe_stall_fgfc_start_0` read on every node before and after each launch. Output
  `~/sess1011c/c02/R4_<epoch>/` (`L01..L12.log`, `nic_*.txt`, `summary.txt`: per launch the MCU group and the per-order rates; per (g, slab, order)
  the mean / min / max over launches and PEs), marker `DONE`. Dry run (`STUB=1`) passed locally and on the login node (m). Launch form:
  `srun --jobid=$J -N4 --ntasks=16 --ntasks-per-node=4 -c 48 --distribution=block --gpus-per-node=4 --overlap --mem=0` (mnrun's binding: task k on
  NUMA k → NIC k).
- **Collect:** `cat ~/sess1011c/c02/R4_*/summary.txt` (and `~/aac7q/queue/50_c02_sched4.rc`). ≈ 8 min of hold once the runner has 4 free nodes.
- **Pass rules (decide before the data):**
  1. **`rr` or `win2` ≥ `rot` + 10 % at g = 4, 64 MiB, in the same launches** (paired) → build the engine switch (§4).
  2. **The group-A launches (slow) lose their penalty under `rr`/`win2` but not under `seq`/`rot`** → the slow mode is sender-order incast: build the
     switch and A/B it on 4 nodes with the MCU group logged per run.
  3. `rr` ≈ `rot` ≈ `seq` within 5 % and the A/B gap identical under every schedule → the schedule is not the lever at ≤ 10 nodes; the remaining
     gap is the fixed per-exchange term (XEFF's 10 ms) and the launch mode; close C8 at this scale, keep `winK` as a 576-node question.
  4. `pair` ≥ `rr` → the NIC prefers one stream per direction (unexpected); then the design changes to a synchronised pairwise schedule.
- Fallback item if 4 nodes never free up before the end of the wave: the same item with `NODES=2` runs only g = 2 (1 peer: no schedule
  difference, but still the launch-mode draw); not installed.

#### 4. The engine change for the best candidate (not built; c03 owns `comm_shmem.c` in wave C)

**Switch:** `COMM_SHMEM_PEER_ORDER=rr` and `COMM_SHMEM_PEER_ORDER=win<K>` (values of X1's existing variable; `rot` and unset keep today's behaviour;
default off → digits identical, only the posting order of the same puts changes; data placement is by the receiver's offset word, unchanged).

**Files / functions (all `ecalc/comm_shmem.c`; ≈ 50-70 lines):**
- `comm_shmem_init`: parse `rr` / `win<K>` into `S.porder = 2` + `S.pwin` (K; `rr` = K ≥ n − 1).
- `ready_first(...)`: today it calls `post(p, r, xw, x)` once per ready peer (the whole slab → `put_signalled` → `comm_ofi_write`, which chunks and
  stripes internally). Under `rr` / `win`: keep a per-peer progress `prog[i]` (bytes posted) instead of `done[i]`; among the ready peers (offset word
  arrived) that are open (≤ K), post **one chunk of `COMM_OFI_CHUNK_MB` (4 MiB) to the open peer with the least progress** per iteration; a peer
  is "done" when `prog[i] == its count`; the signal of a peer is recorded with its last chunk (`p->osig[r] = sig` as `put_signalled` does: on the
  OFI path `ofi_flush` already signals per peer when that peer's counter reaches 0, so interleaved chunks need no change there; on the pure-SHMEM
  path the last chunk goes with `shmem_ctx_fence` + the signal word, as the `put_bytes` pieces do today).
- `rf_post_plain` / `rf_post_round`: take `(r, xw, offset, len)` instead of posting the whole count; `rounder` passes the round's chunk bounds.
- `push_peers`: unchanged apart from calling `ready_first` for `S.porder >= 1`.
- Not touched: `comm_ofi.c` (`comm_ofi_write` is already per (peer, offset, length)); the sequence arithmetic; `publish` / `arrive`;
  `comm_layered.c`.
- README: one row for the new values of `COMM_SHMEM_PEER_ORDER`.

**Staged test plan:**
1. `tests/t_a2a_sched` queue item (above) → pass rule 1 or 2.
2. Build the switch; `t_comm` VERIFY at 2 and 4 nodes with `rr` and `win2` (`t_comm --bw 4 5 256 sym` also gives the transport-level A/B: the
   M2-style ABAB × 3, 15 min); `tools/gate.sh --build --nodes 1` and `--nodes 2`; `mnaccept.sh --only unit,e9` with the switch on.
3. 4-node ABBA, base vs `rr` (and `win2` if different in step 1), ≥ 8 rounds (≈ 182-209 s per run → ≈ 2 h for 8 rounds), the MCU group of every run
   from the counters (A13's `snap.sh`) so the slow-mode runs are analysed as their own stratum; stop rule p < 0.05 or CI half-width ≤ 3 s or 12 rounds.
4. Only if step 3 shows ≥ 3 s or a changed slow-mode penalty: 10-node ABBA as XEFF §4 (the 6-node QOS allows it only after the wave).
5. For 576: `win<K>` cannot be measured here; carry the switch to the target kit's `s2chk`/first ≥ 64-node run as an option (K = 16 and 32).

#### 5. Side findings to hand over

- **a02 (model):** the `layv_factor` 0.41 raw ratio in T06 is an INTER2 double count; span-based ratio 0.73-0.86 at 6 nodes (§1.2). The 576-node
  layv penalty (T06: +8.9 s at 0.655, +24 s at 0.41) is therefore closer to its low end (mod).
- **c06 (comm_layered):** `COMM_XSTATS` could print the stage span (Σ `t_call` + last wait) beside Σ `t_post` under INTER2, which would make the
  layv rate directly readable (today it needs the derivation above).
- **The slow mode at 4 nodes costs more than any schedule can win at 4 nodes** (×0.7 on every exchange kind vs ≤ 19 % gap to the NIC in the fast
  mode); a14's launch-group work is the bigger lever there.

#### RESUME

- Branch `c02` (from main bc80001), commits: `605cdc5` (probe + item), this file. Pushed. aac7 worktree `~/ntt-wt/c02` holds the built probe
  `ecalc/tests/t_a2a_sched` (login-node build); scratch `~/sess1011c/c02/` (`t_a2a_sched` first build, `R4_*` outputs when the item runs).
- Queue item `~/aac7q/queue/50_c02_sched4.sh` installed 16:16 EDT, not yet run (4 nodes busy: b02 2, b06 3, b08 1, c03 gate 2 at 16:16). Collect
  with `cat ~/sess1011c/c02/R4_*/summary.txt; cat ~/aac7q/queue/50_c02_sched4.rc`; then apply §3's pass rules and append a "Microbenchmark result"
  section here.
- Nothing of mine runs on aac7 (no job submitted by me; the runner owns the hold when the item runs). Remove `~/ntt-wt/c02` after merge.
- Open: the pass-rule decision; the engine switch (c03's file); `winK` at ≥ 64 peers only on the target.

## XEFF: why ecalc's exchanges underuse the transport, and what to change

(former file: XEFF.md)

Design study, 2026-10-08. No code changes, no runs. Read-only inputs: aac7 `~/s22/log/scale_n10_r1.log` (S22, 10 nodes, the
`MN_WAIT_STATS` / `MN_COMM_MARK` build) and `~/s19a/log/p10.log` (S19A, 10 nodes, `COMM_LAYER_STATS=1`). Also results/S19B.md, archive:results/S20.md,
S22.md, OFI17.md, TUNE17.md, NIC16_experiments.md, I.md, RESULTS §112 and §116–§122. Code: `ecalc/comm_ofi.c`, `comm_shmem.c`,
`comm_layered.c`, `rns_dist.c` (`mn_core`, `redistribute`, `gen_fwd`, `gen_inv_pw`, `mn_round_add`), `ntt_dist.c`, `mn.c` at
`main` f09e2de. Models: `ecalc/estimate.py`.
Labels: **(m)** measured, **(d)** derived by arithmetic from measured numbers, **(mod)** modelled, **(a)** assumed.

#### 0. Summary

- **Most of the time is spent on bytes the transport could move faster.** Per APU thread at 10 nodes (S19A p10, node 0), the SHMEM
  exchanges take 276.7 s post-to-completion for 1675 GB, an average of **6.05 GB/s** (m). That equals the model's fitted effective rate
  (`aac7_s18`, B = 6). The time divides into two populations:

| population | bytes / APU thread | time / APU thread | rate | what it is |
|---|---|---|---|---|
| layered transform exchanges (inter stage) | 1414.5 GB (84 %) (m) | 140.3 s (m) | **10.1 GB/s** (m) | the 3·np all-to-alls per product; back to back on the wire ("held" 84–92 s per node, m) |
| direct mesh-d exchanges (operand redistribution, result out, spills, `mdb_add_shifted`) | ≈ 261 GB (16 %) (d) | ≈ 136 s (d) | **≈ 1.9 GB/s** (d) | blocking `alltoallv` + immediate `wait`, one sync point each, ≈ 1176 per thread (d) |

- **Two gaps, of similar size:**
  1. **Direct exchanges (≈ 110 s per thread above the layered rate, d).** They carry 16 % of the bytes and take 49 % of the exchange
     time. Each one is a group-wide sync point. It absorbs the skew of the work before it, plus per-exchange fixed costs and the lockstep
     rounds. Skew is bounded at ≤ 68 s per thread at 10 nodes (S22 node spread, m). The split between skew and protocol is **not
     measured**.
  2. **Layered exchanges (≈ 80 s per thread, mod).** They run at 10.1 GB/s against 23.3 GB/s raw per NIC (NIC16, m). Decomposition
     per GB per APU thread (mod, §1.2):

| step | ms per GB | effective rate |
|---|---|---|
| raw NIC | 43 | 23.3 GB/s |
| + comm_ofi's per-peer protocol (delivery-complete writes, drain and handshake per exchange) | +14 | 17.5 GB/s |
| + 9-peer contention (every sender walks its peers in the same order) | +26 | 12 GB/s |
| + per-exchange tail and lockstep rounds inside ecalc | +14 | 10.1 GB/s |

- **Staging copies are not the problem.** Device-to-device copies into the fine-grained comm pool run at ≈ 1.6 TB/s, and kernels on
  fine-grained memory run at full HBM speed (I.md, m). The two staging copies cost ≈ 1.3 ms per GB, about 1.4 % of the wire time (d).
- **The ranked plan:**
  1. A stats-only switch, `COMM_XSTATS=1` (§3), to split the direct-exchange time into skew and protocol per call site and per APU
     thread.
  2. **Rotated, ready-first peer order** in comm_shmem (`COMM_SHMEM_PEER_ORDER=rot`). Small change, low risk.
  3. **Two inter exchanges in flight per mesh, with unstaged v-slots** (`COMM_LAYER_INTER2=1` + `COMM_LAYER_VSLOT_POOL=1`). Medium
     effort, memory-neutral.

  Items 2 and 3 together: about −30…−45 s at 10 nodes (mod, central). At 576 nodes: −60…−95 s on an aac7-class fabric, −8…−12 s on the
  target's standing estimate, and possibly 15–40 s more there from per-exchange fixed costs, which do not shrink with bandwidth (a; §2).

#### 1. Where the gap comes from (10 nodes unless stated)

##### 1.1 Message sizes, counts, rounds (m unless marked)

S22 `scale_n10_r1` (config A: `DM_MN_LEAN=1`, DC8, T1024; with stats). Comm-mark totals are node sums; "per thread" = /4 (a: four APU
threads).

| phase (g) | exchanges (node) | GB received (node) | MB per exchange | MB per peer | post→completion s per thread | GB/s per thread | phase wall s |
|---|---|---|---|---|---|---|---|
| tree level 1 (g = 2, power of two) | 2208 | 583.6 | 264 | 264 | 13.6 | **10.75** | 86.7 |
| tree level 2 (g = 10, general map) | 5308 | 1978.6 | 373 | 41 | 76.7 | 6.45 | 112.5 |
| reciprocal (g = 10) | 9400 | 1078.3 | 115 | 12.7 | 64.0 | **4.21** | 78.7 |
| division (g = 10) | 8312 | 2790.7 | 336 | 37 | 143.5 | 4.86 | 170.8 |
| whole run, pe 0 | 25228 | 6431 | 255 | | 297.8 | 5.40 | total 456.3 |

- **Every exchange takes the rounds path** (`COMM_SHMEM_ROUND_MB=1024` is on the line). All 25228 go through it. 580–668 per node need
  more than one round (3392–3948 rounds in all). The round chunk is 1024 MiB / (n − 1) per pair: 114 MB at g = 10, but **1.78 MB at
  g = 576** (d), where every pair above that is cut into lockstep rounds over 575 peers.
- **Sizes per layered exchange** (S19A layer-stats, node 0): 88 % of the layered time sits in the ≥ 537 MB/APU bucket at
  **10.26 GB/s**, and the 33–270 MB buckets run at 10.9–11.3 GB/s. The buckets below 17 MB run at 0.06–3.5 GB/s but sum to only
  ≈ 4 s of 149 s (m).
- **The per-exchange rate falls with g at the same bytes per node:** 10.75 GB/s at g = 2, 4.2–6.5 at g = 10 (m). At g = 10 the direct
  exchanges are a larger share, and the reciprocal is many small products.

##### 1.2 The transport itself (comm_ofi under comm_shmem)

| evidence | value | label |
|---|---|---|
| one NIC streaming `fi_write`, 1 MiB, window 64 (nicbw); Cray SHMEM 4 PEs per node | 23.3 GB/s per NIC; 23.2 | m (NIC16 #4, #21) |
| `t_comm --bw` all-to-all through comm_shmem + comm_ofi (host-only build: host pool, CPU `memcpy` of the self slab) | 7.7 / 12.3 / 11.0 GB/s per thread at 2 / 4 / 10 nodes | m (TUNE17, OFI17) |
| same, SHMEM path through one NIC (cxi0) at 2 nodes | 19.8 GB/s per node | m (OFI17) |
| chunk 1–8 MiB × window 16–128 | flat (40–48 GB/s per node) | m (TUNE17) |
| fit T = b/c + (n − 1)·b/w to the 2- and 4-node points (c: the self-slab memcpy) | w ≈ 17.5 GB/s, c ≈ 14 GB/s; at 10 nodes w ≈ 12 | mod |

- **Reading:** a single-peer stream through comm_ofi reaches about 17.5 GB/s, not 23.3 (mod). With 9 peers it drops to about 12 (mod).
  The knobs are flat, so the loss is in the pattern and protocol, not the window. The code points:
  - **Same peer order on every sender.** `push_peers` and `rounder` loop `r = 0 .. n−1` (comm_shmem.c `push_peers`, `rounder` "the
    sender's half"). All senders aim at receiver 0 first, then 1, and so on: a moving incast hot spot.
  - **Head-of-line blocking.** The sender `wait_ge`s each receiver's offset word **in that order** (`push_peers` with `spin_us < 0`,
    which is always the case for OFI: `start_push` always uses the helper thread). One late receiver stalls the posts to every later
    peer, although they are ready.
  - **Delivery-complete on every 4 MiB write** (`open_nic`: `FI_DELIVERY_COMPLETE`). The signal goes only after the peer's last write is
    delivery-complete (`ofi_flush`), then as a **Cray SHMEM put through the process's single PE NIC (cxi0)**. So the offset and signal
    words of all four meshes share APU 0's NIC with mesh 0's bulk data.
  - **One exchange at a time per communicator** (`s_alltoall`: "alltoall while one is pending" dies; comm_layered: "the inter transport
    takes one exchange at a time"). Each exchange ends with a drain (the last write's ack, the signal, the receiver's `arrive`, `quiet`).
    The next exchange's first write waits for the next offset handshake. Inside each exchange, the tail (waiting for the slowest pair) is
    idle egress.
- **The ecalc layered rate (10.1–10.3 GB/s) against the t_comm wire estimate (12) at 10 nodes:** about 14 ms per GB (mod). For the
  ≥ 537 MB exchanges: 72.6 ms each for 0.745 GB (d), against ≈ 62 ms at 12 GB/s, so **≈ 10.5 ms of fixed cost per exchange** (mod).
  This is the tail, the drain, the rounder thread, two `hipStreamSynchronize`s and the copies (≈ 1 ms).

##### 1.3 Depth and overlap

- Layered depth 2: exchange k+1's intra (xGMI) stage and transposes run under k's wire. xGMI is 13.9 s per thread at 94.5 GB/s, of
  which 6.7 s is overlapped and 7.3 s exposed (m, S19A).
- **The inter stage is strictly serial per mesh.** "held 86.5–94.3 s" per node (m) means exchange k+1 is ready but waits for k. The
  fabric is busy 94.7 % of the span (m), but "busy" here means "an exchange is pending", which includes the tail and drain of §1.2.
- `COMM_ALLTOALLV_DEPTH > 2` cannot help without a second inter communicator: the transport takes one at a time (B11: depth 1 only at
  K = 1, m).

##### 1.4 Pack / unpack copies and staging

- The path of one v-exchange byte has five HBM copies:
  1. reorder into `x0` (if not contiguous);
  2. xGMI push into `x1`;
  3. reorder into `x2`;
  4. staging copy-in to the comm pool, then the wire, then staging copy-out into `x3`;
  5. scatter into `rb`.
- At 1.5–3 TB/s (I.md, X13b, m) this costs ≈ 3 ms per GB against ≈ 97 ms per GB on the wire: **about 3 %** (d). The block transposes
  take 2.8 s per APU in all (m).
- **Staging is not a bandwidth cost but a structural one.** `COMM_SHMEM_ROUND_MB` exists only to bound the staging. It turns every big
  exchange into lockstep rounds (copy-in → wire → all signals → copy-out per round), and at g = 576 the rounds are 1.78 MB per pair
  (§1.1).

##### 1.5 Skew (waiting for peers)

| evidence | value | label |
|---|---|---|
| S22 n = 10, node spread (max − min) of `comm_wait` per thread: tree / reciprocal / division | 14 / 26 / 28 s (Σ 68 s) | m |
| same at n ≤ 4 | 0–20 s | m |
| reciprocal's single-node chain: 2.28 s minimum, but 6.8–22.2 s typical | the excess is a wait | m (S19B §2) |
| the same ranks wait most in both arms (dm: ranks 0–2; bs: ranks 8–9) | structural | m (S20) |

- The S22 "ready" counter mixes the offset-word waits (a peer is not there: skew) with the signal waits (data in flight: transfer), so
  it cannot split them (S22 §2.1). **The direct exchanges absorb all of it**, because each one is the first sync point after local work:
  gathers, CRT, `mn_round_add`, or the reciprocal's single-node chain.

##### 1.6 Serialization between APU threads, and NIC binding

- **Mesh imbalance:** on every node, the four APU threads' layered fabric time differs by **max/min 1.10–1.18** at identical bytes (m,
  S19A, all 10 nodes; e.g. node 0: 129.9 .. 153.6 s).
- The four threads are coupled at every v-exchange, through the intra (xGMI) stage and the count all-gather in `vtab_build`
  (`comm_allgather_host` over the intra communicator per exchange). So the node runs at its **slowest mesh's** pace: up to ≈ 15 % of
  the layered fabric time (≈ 20 s per thread) is the faster threads waiting (d; upper bound).
- Which APU index is slow is not printed. Suspect: mesh 0, whose NIC (cxi0) also carries every mesh's SHMEM control words (§1.2) (a).
- **NIC binding is correct for data on aac7:** comm_ofi takes the cxi device on each APU's NUMA node, one per APU (code `nic_list`).
  Only the control plane is pinned to one NIC per node (Cray OpenSHMEMX: one NIC per PE, B1).
- **One-thread windows:** under `MN_T_CHUNK_MB`, every T round does a result exchange, then a spill exchange, an `omp barrier`, and
  `mn_round_add` on **one** thread while the other three APU threads and the wire idle (`mn_core`). The 2048 gain (−42 s, m) is local
  by S20's evidence. Its mechanism is still unexplained, and this window is a candidate (a).

##### 1.7 The direct exchanges in numbers (S19A p10, node 0; d)

- Count: comm_shmem 16492 (node) − layered 11788 (node) = 4704, i.e. ≈ 1176 per thread.
- Size and time: ≈ 222 MB average, ≈ 116 ms each, where the layered rate would take ≈ 22 ms. That leaves **≈ 94 ms per direct exchange**
  of skew plus fixed costs.
- What a product does per APU thread (`mn_core`):
  - three blocking redistributions (A, B, X), each preceded by its pack and `hipStreamSynchronize`;
  - Kt × (result `alltoallv` + spill `alltoallv` + barrier + one-thread add);
  - the carry scans (`node_carry_in`, 1 byte per node over mesh 0).
- None of them overlaps anything.

#### 2. Ranked candidate changes

**Conventions:**
- S = seconds saved per APU thread of exchange time at 10 nodes (mod or a, as marked). At 10 nodes S ≈ wall seconds (the exchanges are
  exposed: S19B, S22); `estimate.py`'s own slope is 1.14·S.
- Translation to 576 nodes (mod, assuming the fraction of exchange time saved carries over): `estimate.py` gives
  wall = 176.1 + 3429/B (aac7-class) and 146.2 + 3422/B (target-m), fitted to `--bw 6/7/8/10` and `--bw 28/33/40/47`.
  - aac7-class at B = 6: Δ576 ≈ **2.07·S**.
  - Target standing estimate (B = 47): Δ576 ≈ **0.26·S**; derated (B = 28): **0.44·S**.
- **The proportional rule understates fixed-cost fixes on the target.** Per-exchange tails and handshakes do not shrink with bandwidth.
  At 576 there are ≈ 2.5–4 k exchanges per APU thread (mod, S19B message count / 575), so 5–10 ms per exchange (a) is 12–40 s on the
  target. Items marked **F** below attack that term.

| # | change (switch, off by default) | S at 10 nodes | 576, aac7-class | 576, target-m / derated | memory | effort | risk |
|---|---|---|---|---|---|---|---|
| **0** | **`COMM_XSTATS=1`**: per-exchange phase timers (measurement only; §3) | 0 | 0 | 0 | 0 | S (≈ 150 lines) | none (stats) |
| **1** | **`COMM_SHMEM_PEER_ORDER=rot`**: rotated peer order (peer i = (me + 1 + i) mod n) and ready-first posting (post to any peer whose offset word has arrived, poll the rest) in `push_peers` and `rounder`; signal and copy-out per peer as it arrives | **10–40 (central 20)** (a; recovers part of the 26 ms/GB contention term and the head-of-line blocking in skewed direct exchanges) | −20…−83 (central −41) | −3…−10 / −4…−18 (F: removes head-of-line blocking, which grows with peers) | 0 | **S** (≈ 60 lines, comm_shmem.c only) | **low**: per-pair sequence numbers already make the order free; data placement is by the receiver's offset, so digits are identical |
| **2** | **`COMM_LAYER_INTER2=1` + `COMM_LAYER_VSLOT_POOL=1`**: the v-slots (x2/x3) from the comm pool (`comm_sym_alloc` on the inter communicator: unstaged, no rounds); a second SHMEM communicator per mesh so v-exchange k+1's inter stage posts while k's tail completes ("posting k completes k−1" kept for the callers) | **10–25 (central 18)** (mod: the ≈ 10.5 ms per-exchange tail on ≈ 1824 large layered exchanges per thread, plus the lockstep rounds of v-exchanges) | −21…−52 | −3…−7 / −4…−11; **F**: also removes the 1.78 MB-per-pair lockstep rounds at g = 576 on the 192 and 576 levels (the largest, S19B) | **neutral**: the v-slot term (1.72 GB per APU at DC8, m) moves from `hipMalloc` into the comm pool; no new staging. Option: then `COMM_SHMEM_ROUND_MB=512` for the remaining direct exchanges, −1 GB per APU (mod) | **M** (comm_layered.c, mn.c ids, binsplit pool rule + mem_model `--check-c`) | medium: a new comm id range (trap B9), pool fragmentation (allocate the slots once at the layout's maximum) |
| 3 | `MN_REDIST_FUSE=1`: one `alltoallv` for A, B, X's redistribution (segments back to back per peer), instead of three blocking ones | 3–12 (a; 2 of 3 sync points per product, ≈ 120 per thread × the ≈ 94 ms excess, of which an unknown part is skew that just moves) | −6…−25 | −1…−3 / −1…−5 (F) | 0 (A's and B's receive buffers already coexist; the sends sum) | S–M (rns_dist.c `redistribute`, `mn_core`) | low–medium (P24 path, cache partial hits) |
| 4 | `COMM_OFI_SIG=1`: data writes transmit-complete; per peer, one 8-byte write with `FI_FENCE \| FI_DELIVERY_COMPLETE` into a signal area in the receiver's comm pool, on the device's own NIC; offset words likewise. This takes the control plane off cxi0 | 10–30 (a; the 14 ms/GB protocol term plus the mesh imbalance, if mesh 0 is the slow one) | −21…−62 | −3…−8 / −4…−13; **F at 576**: 575 × 2 control words per exchange per mesh leave the single PE NIC | 0 (+ a few KB of signal area per communicator) | M–L | medium–high (cxi `FI_FENCE` semantics, CPU polling of device memory, ordering proofs); **gated on M2** |
| 5 | `COMM_SHMEM_RPIPE=1`: rounds pipelined two deep (round j+1's offsets and writes go to ready peers while round j's slow pairs finish); staging split into two halves of the same budget | 5–15 (a; direct exchanges in more than one round: ≈ 165 per thread, ≈ 970 rounds) | −10…−31 | −1…−4 / −2…−7 (F) | 0 (same `ROUND_MB` budget) | M (comm_shmem.c `rounder`) | medium |
| 6 | Skew sources, not the transport: the reciprocal's single-node chain wait (≈ 10 s mean excess, S19B #4); `mn_round_add` on one thread | 5–15 (a) | n/a | n/a | 0 | S–M | low; diagnose with `ECALC_LOG_CLOCKS` and XSTATS first |

**Not recommended (evidence in hand):**
- `COMM_OFI_CHUNK_MB` / `WINDOW` tuning: flat (TUNE17, m).
- Two NICs per APU on aac7: no gain, one NIC per APU there (TUNE17, m).
- `DIST_MN_SYM_SLABS=1`: +3q of comm pool per APU (≈ 10 GB per APU at the cap, mod) to save copies that cost about 1 % (§1.4).
- `MN_TOPO_GROUP`: aborts under SHMEM and is inert (B9).
- More `DIST_CHUNKS`: 2× messages, time-neutral (S19B / S20, m).

**Order of work:**
1. 0 (`COMM_XSTATS`) and 1 (peer order) can be built together: 1 is independent of what 0 finds.
2. Run M2 (t_comm A/B, minutes) and then the 10-node ABBA of 1.
3. Build 2 next.
4. 3, 5 and 6 only if XSTATS shows their populations are large: redistribution sync time, multi-round direct exchanges, skew at the
   reciprocal's start.
5. 4 only if M2 shows delivery-complete costs more than 15 % on a single peer stream.

#### 3. Measurements needed first (cheap)

##### M1 `COMM_XSTATS=1` (stats only, off by default, digits unaffected; build in the same branch as item 1)

**Where:**
- `comm_shmem.c`: `s_alltoall`, `s_alltoallv`, `alltoallv_rounds` / `rounder`, `push_peers`, `ofi_flush`, `arrive`, `s_wait`.
- `comm_util.c` / `comm.h`: next to the D3 `comm_wst_*` phase machinery (reuse its phase: bs / dm / recip / other).

**Per exchange, record:**
- `t_post`: entry;
- `t_staged`: copy-in done;
- `roff_wait`: Σ blocked in `wait_ge` on `W_ROFF` words = **skew**;
- `t_lastpost`: the last `comm_ofi_write` returned;
- `t_flushed`: `ofi_flush` returned (my data delivered);
- `sig_wait`: Σ blocked in `wait_ge` on `W_SIG` words = **transfer and the tail**;
- `copyout`;
- `t_waitcall`: when the caller entered `s_wait` (so "caller late" is told from "wire busy");
- `t_end`.

Plus bytes, rounds K and peers.

**Tags:**
- The **kind** is a thread-local `comm_xtag` set by the callers before posting and captured into `shm_priv` at post time (the rounder
  runs in a helper thread):
  - `comm_layered.c` `inter_post` → LAYER_EQ, `v_post` → LAYER_V;
  - `rns_dist.c` `redistribute` → REDIST, the result `alltoallv` in `mn_core` → RESULT, the spill exchange → SPILL,
    `mdb_add_shifted` → ADDSH;
  - default OTHER.
- The **APU index** comes from `hipGetDevice` at post time.

**Output** (at each `MN_COMM_MARK` point and at exit, node 0 plus the max/min over nodes like `mn_wait_stats_print`): one line per
(phase, kind) with count, GB, the sums of each interval and K. One line per APU index with the inter-stage seconds and GB, to name the
slow mesh. Split the existing S22 `ready` counter into `ready_roff` / `ready_sig` at the same call sites (10 lines).

**Run:** one 10-node run on the aac7 base line + `COMM_XSTATS=1 MN_WAIT_STATS=1 MN_COMM_MARK=1` (one run, ≈ 8 min; compare shapes, not
levels: the stats cost 3–4 % in S22).

**Decides:**
- what share of the ≈ 110 s per thread of direct-exchange excess is skew (`roff_wait`) versus tail and protocol (`sig_wait`), by kind;
- whether the slow mesh is APU 0 (→ item 4);
- how many direct exchanges are multi-round (→ item 5).

##### M2 t_comm transport A/B (≈ 15 min on the hold, no ecalc)

- `t_comm --bw 4 5 256` (sym) at 2, 4 and 10 nodes, `COMM_OFI_POOL_MB` sized for 10 peers (§117 lesson), ABAB × 3, with
  `COMM_SHMEM_PEER_ORDER` = 0 / rot.
- Two small additions to `t_comm`, test code only:
  - **`noself`**: skip the self-slab `memcpy` of the host-only build, which caps the 2-node figure (§1.2);
  - a **`COMM_OFI_DC=0` diagnostic** (transmit-complete writes, then one fenced delivery-complete 8-byte write per peer before the
    signal): t_comm VERIFY must pass.
- **Pass criteria:**
  - item 1 is worth an ecalc A/B if the 10-node per-thread rate rises ≥ 10 % (11.0 → ≥ 12.1 GB/s);
  - item 4 is worth building if DC=0 lifts the 2-node single-peer rate ≥ 15 %.

#### 4. The A/B test for each built switch (exact)

**Setup:**
- Hold: 10 nodes, one network program per node, no other job on them.
- Driver: `tools/rundriver.sh`, like `archive:drivers/ecalc/s22_batch.sh`.
- Base line, the aac7 base of 2026-10-08:
```
COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1
MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=2048 DM_MN_LEAN=1 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6
ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 COMM_OFI_PLAN_CXI=1          (DIST_CHUNKS = code default 8)
SLURM_JOB_ID=$J MNRUN_NODES=10 ./mnrun.sh 10 env $LINE [$SW] ./ecalc 644100000000      # 6.441e10 digits/node, no write
```
- A = base, B = base + the switch (item 1: `COMM_SHMEM_PEER_ORDER=rot`; item 2:
  `COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1`; combined: both).
- **ABBA:** odd rounds A first, even rounds B first. Evict `~/ref/e_*` from the page cache before each run (S22's `evict`).
- **Stop rule:** S22's blocks of 4 rounds; stop at |t| > t_crit or 16 rounds (≈ 4.3 h at ≈ 8 min per run). S22's paired sd of 21 s
  gives a 95 % CI of ±11 s at 16 rounds (m), enough for the central 18–20 s.

**Per run, record:**
- `total`, `bs`, `dm`, recip, division;
- the four comm-mark lines;
- comm_shmem pe lines;
- the top-node peak (`MEM_REPORT_DEVS`);
- VERIFY.

**Before arming (gates, the private protocol):**
- `t_newton`, `t_mul`, `e9` identical in both bases, 4 × 10¹⁰ identical to `e_4e10.out`;
- `./mnaccept.sh $J --only unit,e9` with the switch on (layered self-test, general map sizes 3/6/10);
- `t_comm` VERIFY at 2/4/10 nodes with the switch on;
- for item 2: `mem_model.py --check-c` exact with the moved v-slot term, and `MN_PLAN_ONLY=37100000000000:576` with and without the
  switch: `plan pool` and the node / device totals must not grow.

**Adopt-or-not evidence:**
- paired B − A (mean, median, sign test);
- the per-phase comm-mark GB/s per thread (it should rise at g = 10);
- XSTATS (if on): `roff_wait` and `sig_wait` per kind;
- peak memory unchanged (item 1) or within ±0.5 GB per node of the moved term (item 2).

#### 5. Implementation notes for the top three (for a sonnet agent)

##### Item 0, `COMM_XSTATS`

As M1 above.
- **Files:** `comm.h` (the tag enum, `comm_xtag_set(int)` thread-local), `comm_util.c` (sums under the D3 phase), `comm_shmem.c`
  (timers), `comm_layered.c` and `rns_dist.c` (two to four `comm_xtag_set` calls each), `mn.c` (printing next to
  `mn_wait_stats_print`).
- **Off-path cost:** one flag test.
- **README:** one row for `COMM_XSTATS`.

##### Item 1, `COMM_SHMEM_PEER_ORDER=rot` (`comm_shmem.c` only)

- Add `S.porder` (env, default 0) and `static int peer_at(const shm_priv *p, int i)` = `(p->me + 1 + i) % p->n` for i < n − 1.
- **`push_peers`:** when `S.porder`, keep a `char *pend` per call. Loop over the pending peers in rotated order with
  `test_ge(W_ROFF)`; `put_signalled` each ready one; after a pass with none ready, `sched_yield`. The `spin_us >= 0` inline variant
  returns the first not-ready index as today; it is never taken under OFI.
- **`rounder`, the sender's half of round j:** the same ready-first rotated loop.
- **`rounder` and `arrive`, the signal waits:** under the switch, poll `test_ge(W_SIG)` over the pending peers and start each peer's
  copy-out as soon as its signal arrives. Correctness only needs all of them before the `quiet`.
- **`ofi_flush`:** unchanged (it already signals per peer as soon as that peer's counter reaches 0).
- **Do not change:**
  - `put_word` / `publish` order (irrelevant);
  - the sequence arithmetic;
  - comm_ofi's NIC striping `j = (i + r) % k`.
- **Tests:** `t_comm` VERIFY with the switch at 2/4/10 nodes; `mnaccept unit,e9`; e9; 4e10; then M2 and the §4 A/B.

##### Item 2, `COMM_LAYER_VSLOT_POOL=1` then `COMM_LAYER_INTER2=1`

- **VSLOT_POOL:**
  - In `need_vslot`, when the inter communicator gives `comm_sym_alloc` memory, allocate each slot **once** at the layout's maximum.
    `binsplit_vslot_bytes` / B7ACCT's `A + max(A, B) + 4` at the plan's largest q avoids grow-and-fragment in the first-fit pool.
  - Free with `comm_sym_free`.
  - In `comm_shmem.c` `alltoallv_rounds`: `if (sin && rin && <switch>) return 0;`, because rounds only bound staging.
  - Pool rule: add 2 slots to the comm pool's need per APU, and remove the same term from the `hipMalloc` v-slot accounting, in C
    (`binsplit.c`) and `mem_model.py`.
- **INTER2:**
  - `mn.c`: create a second SHMEM communicator per (level, d) from a fresh id range, checked against `MAXID` 1024; trap B9 reused
    `NA + NA·(2l+1) + d`.
  - `comm_layered.c`: hand it to the layered communicator as `p->inter2`. Exchange k uses `inter[k & 1]`. Turn `struct lay_v` into a
    ring of two pending entries.
  - In `y_alltoallv2`, the order becomes `v_stages(k)` (slot k & 1, free because k−2 completed), then `v_post(k)` on `inter[k & 1]`,
    then `complete_v(k−1)`. "Posting k completes k−1" stays true for `gen_fwd` / `gen_inv_pw`.
  - `complete_v` before any equal-slab exchange and in `y_wait` must drain both entries.
  - Require VSLOT_POOL (fatal otherwise: two staged inter exchanges would double the staging).
  - The equal-slab path (power-of-two g) can follow later in the same way.
- **Tests:**
  - `mn_selftest_layered` at sizes 3, 6, 10 (the general map) and 2, 4, 8;
  - `t_dist`;
  - `mnaccept unit,e9`;
  - e9;
  - 4e10;
  - `--check-c`;
  - `MN_PLAN_ONLY :576`.

#### 6. Decisions for the user

| decision | why now | options | cost / benefit |
|---|---|---|---|
| X1: build items 0 + 1 (`COMM_XSTATS`, rotated ready-first order) | the stats decide which later items are worth building; item 1 is the cheapest transport fix and matters most at 576 (head-of-line blocking grows with peers) | (a) build both (one branch, S effort), then M1 + M2 + the 10-node ABBA; (b) item 0 only; (c) neither | (a) ≈ 1 day of agent work + ≈ 5 h of hold; −10…−40 s at 10 nodes (a), −20…−83 s at 576 aac7-class (mod); no memory; low risk. (b) data only (≈ 1 h of hold). (c) the 110 s per thread of direct-exchange time stays unexplained |
| X2: build item 2 (inter2 + unstaged v-slots) after X1's data | it is the only item that removes the lockstep rounds at g = 576 (1.78 MB per pair) | (a) build (M effort) and A/B; (b) wait for target wait-stats (TARGET_TASKS T12 `s2chk`) | (a) −10…−25 s at 10 nodes (mod), memory-neutral, medium risk (ids, pool layout); (b) no cost, but the 576 round structure stays untested |
| X3: item 4 (OFI-native signalling) | only if M2 shows the delivery-complete cost | (a) gate on M2; (b) build now | (a) avoids an M–L, higher-risk build if delivery-complete is cheap; (b) up to −30 s at 10 nodes (a), and the control plane leaves cxi0 at 576 |

Decisions X1–X3 are proposals. No switch here exists yet; every one would be off by default, and adoption is the user's decision on
measured data.

#### Reproduce (bounded reads)

```
R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 aac7"
$R 'cd ~/s22/log; grep -E "^comm-mark|^wait-stats node 0|^total|^comm_shmem: pe [0-9]+: all-to-all" scale_n10_r1.log | cut -c1-400'
$R 'cd ~/s19a/log; grep -E "layer-stats.*node 0" p10.log | cut -c1-400; grep -E "NIC balance|one-at-a-time" p10.log | cut -c1-300'
cd ecalc; for b in 6 7 8 10; do python3 estimate.py --fabric aac7_s18 --bw $b --D 6.441e10 --g 10 576 | grep -E "^(10|576) "; done
for b in 28 33 40 47; do python3 estimate.py --fabric target-m --bw $b --D 6.441e10 --g 576 | grep -E "^576 "; done
```
Model values used (mod):

| model | B (GB/s per APU) | wall |
|---|---|---|
| aac7_s18, 10 nodes | 6 / 7 / 8 / 10 | 435.3 / 390.3 / 356.5 / 309.3 s |
| aac7_s18, 576 nodes | 6 / 7 / 8 / 10 | 747.6 / 666.1 / 605.0 / 519.0 s |
| target-m, 576 nodes | 28 / 33 / 40 / 47 | 268.4 / 249.9 / 231.7 / 219.0 s |

#### X3 verdict (2026-10-08, M2 measured): item 4 (OFI-native signalling) is NOT worth building

**Test (measured):** `t_comm --bw 4 5 256 sym`, 2 nodes (aac7-n50, aac7-n60 of hold 12377), host-only t_comm build, comm_ofi on (cxi), base env of §4
(`COMM_SHMEM_DEVHEAP=1 COMM_OFI_PLAN_CXI=1`, pools 8192 MB), ABBA x3 = 6 runs per arm. Arm A = default (data writes `FI_DELIVERY_COMPLETE`); arm B = `COMM_OFI_DC=0`
(new diagnostic on branch s29, `comm_ofi.c`: transmit-complete data writes with no delivery wait and no replacement fence, i.e. an UPPER BOUND for item 4, unsafe: the SHMEM signal may pass the data).
Driver `archive:drivers/ecalc/s29_m2.sh`, branch s29; logs `~/s29m2/` on aac7.

| slab | DC=1 per-thread GB/s (6 runs) | DC=0 per-thread GB/s (6 runs) | lift |
|---|---|---|---|
| 256 MiB | 6.66 mean (6.14-7.63) | 6.67 mean (6.24-7.47) | +0.2 % |
| 64 MiB | 6.78 mean (6.42-7.14) | 6.79 mean (6.45-7.21) | +0.1 % |
| 16 MiB | 7.02 mean | 6.97 mean | -0.7 % |
| aggregate 4 threads, 256 MiB | 24.9 GB/s (mean) | 24.2 GB/s (mean) | within noise |

Run-to-run spread is about +-8 %; both arms scatter identically. `t_comm` VERIFY: OK on both PEs for DC=1 and also DC=0 (no data race observed, not a proof).

**Gate (§3 M2: item 4 worth building if DC=0 lifts the 2-node single-peer rate >= 15 %):** measured lift 0.2 % (an upper bound, since the real item 4 adds a fenced
8-byte write per peer). FAILS by two orders of magnitude. Delivery-complete costs nothing measurable on the bandwidth path; the 14 ms/GB "protocol term" is not the
delivery wait. Item 4's remaining claim is only F (control words leaving cxi0 at 576 PEs), which M2 does not test and which XSTATS (item 0) can show if it matters.
**Decision (per the gate rule): X3 not built; no s29 batch armed.** Switch `COMM_OFI_DC` (default 1 = unchanged) and `MNRUN_NODELIST` (mnrun.sh, default unset) remain on branch s29 only.

## T12 - 4-node uffd soak (FI_MR_CACHE_MONITOR=userfaultfd) on aac7

(former file: T12.md)

Status: **DONE 2026-10-12 (run as item O6, see Results below).** Earlier: paused by user 13:40 EDT 2026-10-11, soak not run. Driver built, dry-run tested, one real run checked; hold 12518 was scancelled by the coordinator.

#### Design
- Question: does the uffd monitor remove the end-of-init crash seen about 1 in 26 real-size 4-node runs on base (S46: memhooks deadlock; uffd 0/104 on the reproducer)? Side output: slow/fast mode counts (T05/T09).
- Code: main 6d80ef1, aac7 worktree `~/ntt-wt/t12`, built with `tools/gate.sh --build --nodes 1 --hold <J>` (gate PASS, 1e9 digits, VERIFY OK, `-DCOMM_SHMEM` present in build.log; measured). The driver runs a private copy of the binary (`OUT/ecalc_t12`).
- Hold: `sbatch -p 192C4G1H_MI300A_RHEL9_A1 -N4 --exclusive -c 192 --gpus-per-node=4 -x aac7-n01,aac7-n61 -t 7:00:00 -J t12soak --wrap "sleep 25200"`.
- Driver `archive:drivers/t12/t12_drive.sh` (+ `t12_stats.py`), login node, back-to-back runs through `mnrun.sh` (SLURM_JOB_ID=hold): 6.441e10 digits/node, e16_headline aac7 LINE + `FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1`, no digit writes. Before each run: page-cache eviction of ~/ref/e_*, MemAvailable >= 428 GB on all 4 nodes, no leftover ecalc.
- Watchdog per run: segv regex (kill 30 s after the text if still alive), 300 s without log growth = hang (ps -L wchan + eu-stack capture in OUT/cap, then kill own PID), 900 s hard timeout. Class: ok / segv / hang / timeout / fail.
- Per run recorded: rc, class, wall, total, init, bs, dm, mean all-to-all GB/s per APU thread, mode (slow < 11.95, fast > 12.3), start time, phase, max log silence.
- Stop: 90 runs, deadline (arm + 6.5 h, clipped to hold end - 20 min; no run starts unless now + 900 s fits), 6 consecutive non-OK runs, 10 consecutive guard failures, or `OUT/STOP`. Then stats (crash-free count, rule-of-three and exact 95 % upper bound on crash rate, slow/fast counts), marker `OUT/T12_DONE`, scancel of own hold (checked by name t12soak and user).

#### Tests done (13:2x-13:38 EDT)
- Stub dry runs (random ok / segv / hang / fail): all classes, captures, 6/3-bad stop, stats and marker behave (measured, local).
- One real run in hold 12518 (driver, MAXRUNS=1, FI_LOG_LEVEL=info): ok, rc 0, VERIFY OK all 4 nodes; log shows "Default memory monitor is: uffd"; total 246.9 s (init 50.1, bs 74.5, dm 117.9), all-to-all 10.15 GB/s per APU thread = slow mode, wall 258 s, hold otherwise idle (measured, first run of a fresh hold).
- Runs take about 260-270 s, so 90 runs need about 6.6 h: expect the deadline, about 85-88 runs (modelled).

#### Arm command (overnight; aac7 login node)
```
J=$(sbatch ... as above --parsable)    # wait for RUNNING
cd ~/t12 && HOLD=$J setsid nohup bash t12_drive.sh > ~/t12/soak.out 2>&1 < /dev/null &
```
(driver files are in `~/t12/` on aac7; OUT default `~/t12/soak`; remove `~/t12/real1` not needed.) Marker: `~/t12/soak/T12_DONE`. Collect: `cat ~/t12/soak/T12_DONE ~/t12/soak/stats.txt; cat ~/t12/soak/ALERT`.
aac7 is Pacific; set the start so no run overlaps anything else of ours. No 00:00-03:30 restriction on aac7.

#### RESUME
Nothing running (squeue empty, no driver). To run: submit the hold, start the driver as above, record PID, return. After finishing: copy stats into this file, add a RESULTS.md section, remove `~/ntt-wt/t12`.

#### Results (O6 run, collected 2026-10-12)
Run: aac7, 4 nodes, main 0298fe2 (gate 1 node PASS), hold 12609, driver rc 0, 05:46-08:16 EDT, ended at the deadline (08:30), not the 90-run cap. The first start (hold 12607, 05:04) failed at once ("worktree not at 0298fe2": the short-hash check failed after the history rewrite); fixed, restarted 05:45 (m). Raw data stays on the cluster (`~/t12/o6/`).

| quantity | value |
|---|---|
| runs | 46: ok 46, segv 0, hang 0, timeout 0, fail 0 (m) |
| uffd monitor (`FI_MR_CACHE_MONITOR=userfaultfd`), 6.441e10 digits/node, no digit output | all 46 VERIFY-clean runs, rc 0 (m) |
| mean total / init / wall | 186.5 s / 31.0 s / 196.0 s; total min 177.9, max 231.9 (m) |
| network mode (all-to-all per APU thread) | fast 39, slow 7; mean total fast 181.9 s (177.9-199.2), slow 212.0 s (204.9-231.9) (m) |
| max log silence in any run | 20 s (watchdog 300 s) (m) |

##### What 46 clean runs bound
- Crash rate: 0/46. 95 % upper bound 6.3 % per run exact (rule of three 3/46 = 6.5 %) (m, computed).
- Earlier rates for comparison: base 4-node real size about 1 in 26 = 3.8 % (S46 section 6.x, quoted there; m); base 2-node 4/422 = 0.95 % (S34, S46/A09; m); S46 reproducer: 0 segfaults in every arm, base 22/105 hangs (21 %), uffd 0/104 (m).
- Chance of seeing 0/46 if uffd changed nothing: at 3.8 %/run (0.962^46) 16 %; at 0.95 %/run 63 % (modelled from the quoted rates). So 46 clean runs are consistent with "uffd removes the crash" but do not show it: the 95 % bound (6.3 %) lies above the earlier 4-node rate (3.8 %). Rejecting a 3.8 % rate at 95 % needs about 78 clean runs (3/0.038, rule of three); rejecting 1 % needs about 300 (assumed).
- Not shown: a difference between uffd and base on the same line (no concurrent base arm). The hang (22/105 on the reproducer) did not occur in any of the 46 (hang bound likewise 6.3 %).

#### RESUME (after O6)
O6 soak finished; nothing running on aac7 for T12 (hold 12609 no longer queued (the driver logged an ALERT 'not cancelling: not our job named t12soak' - the cancel guard did not match the hold's name; the hold is gone anyway)). Open: more clean runs (about 32 more reach the 78-run rule-of-three point for the 3.8 % rate); decision to adopt uffd belongs to the user.
