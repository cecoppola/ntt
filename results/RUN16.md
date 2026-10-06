# RUN16 — today's optimized configuration at near-limit sizes on aac7, 1 node then 10 nodes (SKELETON, not committed)

Driver armed and running in the background on aac7 as of 2026-10-05 18:50 PDT (21:50 EDT). This file is a
skeleton: everything below "## COLLECT" is filled in already (measured/modelled/assumed); everything under
"## COLLECT" is still missing and must be read off the finished run's logs with the exact commands given.
Times Eastern; aac7 is PDT = Eastern − 3 h.

## 1. Commit, build, modules

- Repo `~/ntt-acc` on aac7 refreshed from a stale clone (e5466b7) to **origin/main 5ae3278** via a GitHub
  HTTPS fetch (aac7 cannot reach GitHub by SSH key for `git@github.com:...`, but
  `https://github.com/cecoppola/ntt.git` works): `git fetch https://github.com/cecoppola/ntt.git
  main:refs/remotes/gh/main && git merge --ff-only gh/main`. Commit 5ae3278 = "docs/TARGET_WISHLIST.md v2:
  every measurement still needed from the target, with newbench1's status" (docs-only; same code as e5466b7).
- Build: `source ecalc/aac7env.sh && cd ecalc && make SHMEM_CRAY=1 GMP_HOME=~/gmp -j32` — clean build, `ecalc`
  and `tools/unpack_digits` produced.
- Modules (measured, `aac7env.sh` sourced): `cray-dsmml cray-openshmemx rocm/7.2.4` (confirmed by
  `ldd ecalc | grep libamdhip64` → `/shareddata/opt/rocm-7.2.4/lib/libamdhip64.so.7`). `AAC7_ROCM` default
  used (not overridden to 7.0.3).
- Code defaults in effect on this commit (measured via `ecalc/README.md` grep, all confirmed defaults, not
  set explicitly): `COMM_INIT_EARLY=1`, `MN_SELFTEST_GROW=1`, `BS_POOL_RULE=1`, `RNS_STRATEGY=auto`.

## 2. Job and nodes

- Job **12287** (`HOLDA1`, 10 nodes, `sleep`), RUNNING, `RunTime 02:33:58` at the time checked,
  `TimeLimit 2-00:00:00`. NodeList: x9000c1s0b0n0, x9000c1s1b1n0, x9000c1s2b0n0, x9000c1s3b0n0, x9000c1s3b1n0,
  x9000c1s4b0n0, x9000c1s4b1n0, x9000c1s5b0n0, x9000c1s5b1n0, x9000c1s6b0n0 (measured, `scontrol show job 12287`).
- Jobs 12285/12286/12288 (`HOLD16`, PENDING) and 12287 itself: **never cancelled, never touched** beyond
  `srun --jobid=12287 --overlap` steps the driver issues.
- MemAvailable per node (measured, `/proc/meminfo` via `srun --jobid=12287 -N10 --overlap`, two readings ~40
  min apart — both given):
  | node | reading 1 | reading 2 |
  |---|---|---|
  | x9000c1s0b0n0 | 520 GB | 520 GB |
  | x9000c1s1b1n0 | 518 GB | 518 GB |
  | x9000c1s2b0n0 | 518 GB | 518 GB |
  | x9000c1s3b0n0 | 439 GB | 442 GB |
  | x9000c1s3b1n0 | 520 GB | 520 GB |
  | x9000c1s4b0n0 | 440 GB | 442 GB |
  | x9000c1s4b1n0 | 440 GB | 442 GB |
  | x9000c1s5b0n0 | 520 GB | 520 GB |
  | x9000c1s5b1n0 | 520 GB | 521 GB |
  | x9000c1s6b0n0 | 440 GB | 442 GB |

  **Min MemAvailable = 439–442 GB** (the four nodes carrying the 77 GB Ollama cache the task describes); the
  other six read ~518–521 GB (Ollama apparently not yet paged in on those, or already reclaimed — not
  independently explained here). Budget used throughout: **min MemAvailable − 12 GB ≈ 427–430 GB**, taken
  conservatively as **427 GB** for sizing.
- The 1-node run was launched with `MNRUN_NODES=1` against the job's node list and `mnrun.sh` picked the
  **first** node, x9000c1s0b0n0 (measured 520 GB) — well above the 427 GB budget used to size it (the sizing
  used the conservative 427 GB figure, not this specific node's higher figure, so the actual margin on this
  node is larger than planned).

## 3. Sizes chosen and why

Both sizes were chosen by sweeping `BS_LAYOUT_ONLY` (per-node digits : node count) with the launch
environment on the login node, reading the `room:` line's `node <bytes>` field, against the 427 GB budget.
The code's own `fits` flag in that line checks against a 480 GB design budget (the target's), **not** aac7's
real memory — it is not a safety check for this run; the 427 GB figure from §2 is.

- **1 node: 1.03 × 10¹¹ digits** (`D1=103000000000`). Layout node = **420.33 GB** (measured,
  `BS_LAYOUT_ONLY=103000000000:1`), 6.7 GB under the 427 GB budget. Swept 95–150 × 10⁹: the node-byte figure
  is flat in bands (e.g. 101–103.7 × 10⁹ all give 420.3x GB) with a step to 426.79 GB at 104 × 10⁹ (margin
  only 0.2 GB there — rejected as too tight) and to 428.96 GB at 105 × 10⁹ (over budget). 1.03 × 10¹¹ was
  chosen as the largest round value in the lower flat band, comfortably over 10¹¹ (needed for the prefix
  check) and with real margin (6.7 GB to the 427 GB budget, ~19 GB to the node's true MemAvailable minus the
  12 GB reserve). `plan check 1.03e+11 digits g 1, ECALC_NP=auto: OK — 76 products` (measured, login node).
- **10 nodes: 8.1 × 10¹¹ digits total** (`D10_PER=81000000000` per node × 10 = `D10=810000000000`). Layout
  node = **419.59 GB** (measured, `BS_LAYOUT_ONLY=81000000000:10` with the full launch-line environment),
  7.4 GB under the 427 GB budget. Swept 75–85 × 10⁹ per node: flat at 419.59 GB from 79 × 10⁹ through
  81.6 × 10⁹ per node, stepping to 428.18 GB at 81.8–84 × 10⁹ (over the 427 GB budget) and to 436.77 GB at
  85 × 10⁹. 81 × 10⁹/node (8.1 × 10¹¹ total) was chosen as a round value safely inside the lower flat band.
  `plan check 8.1e+11 digits g 10, ECALC_NP=auto: OK — 96 products`; `plan pool COMM_SHMEM_POOL_MB=8704`
  (staging 8192 MiB = 4 × 2048 MiB by tree level of 2, control 12 MiB; heap ≥ 9216 MiB) (measured, login node).
- Not used: the task's reference point of 8.4 × 10¹¹ at "428 GB" for 10 nodes — that layout point (82–84 ×
  10⁹/node) now measures 428.18 GB against today's 427 GB budget (today's MemAvailable, 439–442 GB min, is
  slightly lower than whatever gave 428 GB fitting before), so it was stepped down one band to 8.1 × 10¹¹.

## 4. Environment of both runs

**1 node** (the multi-node-only switches of the 10-node launch line — `COMM_TRANSPORT`, `COMM_SHMEM_*`,
`MN_OUT_DKM_HI`, `MN_T_CHUNK_MB`, `RNS_DIST_CACHE_FIT`/`PARTIAL`, `MN_TOPO_GROUP` — are inert at g = 1 and
dropped, confirmed by a tiny g=1 test run producing a single `e.out` with no `.part*` suffix):
```
ECALC_NP=auto ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1 MN_COMM_MARK=1
```

**10 nodes** (verbatim, the task's launch line = docs/TARGET.md §4 adapted to aac7 by `e16_headline.sh`):
```
COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 \
RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 \
ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1 MN_COMM_MARK=1
```

Plus the code defaults of §1 (`COMM_INIT_EARLY=1`, `MN_SELFTEST_GROW=1`, `BS_POOL_RULE=1`,
`RNS_STRATEGY=auto`), unset and taking their default values in both runs.

## 5. Driver

`~/ntt-acc/ecalc/run16.sh` on aac7 (copy kept at
`/tmp/claude-1000/-home-machinus/ef912895-d4c4-4bb6-96f5-47d0cecdd8b7/scratchpad/run16.sh` in this session's
scratchpad), modeled on `ecalc/e16_headline.sh`, launched under `setsid nohup` inside job 12287
(`SLURM_JOB_ID=12287 setsid nohup ./run16.sh > ~/p16/R16/log/nohup.out 2>&1 < /dev/null &`), started
2026-10-05 18:50:29 PDT (21:50:29 EDT). It never cancels 12287 and never touches 12285/12286/12288.

Steps, each idempotent (a `*_DONE` marker file in `~/p16/R16/` skips a finished step on rerun — safe to
resume if a step fails on the known ~13 % Cray `shmem_init_thread` init segfault, A16 §4, which this driver,
unlike `e16_headline.sh`, does **not** auto-retry; a failed launch must be rerun by hand: `cd ~/ntt-acc/ecalc
&& SLURM_JOB_ID=12287 ./run16.sh` picks up where it stopped):

1. **plan** (both sizes): `MN_PLAN_ONLY` + `BS_LAYOUT_ONLY`, login-node only, no device — §3's numbers.
2. **n1 record**: `mnrun.sh 1` (`MNRUN_NODES=1`) → `~/p16/R16/n1/e.out` (+ `.t1` sidecar), 1.03 × 10¹¹ digits.
3. **n1 RECHECK**: the same launch with `ECALC_RECHECK=1` against the written file (packed; full residue
   check, no ASCII).
4. **n1 prefix**: `unpack_digits` on the single `e.out` (g = 1 writes one file, no `.part*`) → `e.txt`, then
   `cmp` of the first 100000000002 bytes against `~/ntt/ecalc/results/e_1e11.out` (**the 10¹¹ reference**,
   confirmed present, 100000000003 bytes).
5. Delete `n1/e.out`, `.t1`, `.txt`; keep `~/p16/R16/log/*`.
6. **n10 record**: `mnrun.sh 10` → `~/p16/R16/n10/e.out.part0000..0019` (`MN_OUT_DKM_HI=1`: 2 parts/node ×
   10 nodes) + `.t1`, 8.1 × 10¹¹ digits total.
7. **n10 RECHECK**: `ECALC_RECHECK=1` against the packed parts (full residue check across all 10 nodes,
   no ASCII) — expects `mn: all 10 nodes: RECHECK OK`.
8. **n10 prefix**: unpacks **only** `e.out.part0000`, `0001`, `0002` (not all 20 parts) to ASCII, concatenates,
   `cmp`s the first 100000000002 bytes against the same reference.
9. Delete all of `n10/e.out.part*`, `.t1`, the 3 ASCII parts; keep `~/p16/R16/log/*`.
10. Done marker: `~/p16/R16/RUN16_DONE`; walls appended to `~/p16/R16/log/walls.txt` and
    `~/p16/R16/log/run16.log`.

### Deviation from the task's template (and why)

`ecalc/e16_headline.sh`'s "chain" step does a **full** ASCII RECHECK: it unpacks every `.part*` file (not
just the prefix) and RECHECKs the full ASCII form. `/shared/midgard/home` (NFS) had only **798 GB free**
(measured, `df -h`) when this was sized — not the 2.7 TB the E16 script's own comment assumes. At
8.1 × 10¹¹ digits, full ASCII is ≈ 754 GB (1 B/digit) on top of the ≈ 360 GB of packed parts (0.444 B/digit)
— over 1.1 TB together, which does not fit. This driver instead: (a) RECHECKs the **packed** file/parts in
full (a complete residue/windows/T1 check, no ASCII involved — this is the "VERIFY" the task asks for, beyond
the run's own inline `VERIFY OK`), then (b) unpacks only the first 3 of 20 parts (≈ 3 × 4.05 × 10¹⁰ =
1.215 × 10¹¹ digits, enough to cover the 1 × 10¹¹ + 2 byte prefix) for the reference comparison. It does
**not** RECHECK the full ASCII form of the 10-node run. The 1-node run's ASCII form (1.03 × 10¹¹ digits ≈
103 GB) is small enough that it is converted and could be RECHECKed in full if wanted, but this driver only
unpacks it for the prefix compare, consistent with the 10-node run.

Naming conventions confirmed by small test runs before the real sizes were launched (deleted after
confirming): at g = 1, ecalc writes a single `<name>` + `<name>.t1` (no `.part*`); at g = 10 (with
`MN_OUT_DKM_HI=1`), `<name>.part0000` .. `.part0019` + one shared `<name>.t1`; `unpack_digits -q -o out.txt
packed.file` finds the sidecar automatically by replacing the packed extension; part 0000 is confirmed to
hold the leading digits ("2.718281828459045235360287471352662497757247093699959574966967627724076630353547"
from a 10-node, 10 000 000-digit test). `RECHECK OK` (g=1) / `mn: all N nodes: RECHECK OK` (g>1) are the
pass strings.

## 6. What is NOT yet known (filled in only after the run finishes)

Not yet available: both walls (`total`, with/without the disk write is not separated by this driver — see
COLLECT), the phase breakdown (init/batch/top levels/distributed levels/reciprocal/division from
`ECALC_VERBOSE=2`'s per-phase lines), init time, the `comm-mark` lines (`MN_COMM_MARK=1`, 10-node run only —
inert at g=1), peak memory per node vs the §3 model, the VERIFY and RECHECK pass/fail lines, the prefix
verdict for both sizes, and the clocks (`ECALC_LOG_CLOCKS=1`, `aac7env.sh --log` lines: per-APU sclk/mclk/fclk
and the APU→NUMA→cxi map).

## COLLECT — exact commands to fill in §6, once `~/p16/R16/RUN16_DONE` exists

Check the marker first:
```
sshpass -p <cluster-password> ssh chcoppola@aac7.amd.com "ls -la ~/p16/R16/RUN16_DONE ~/p16/R16/N1_PREFIX_DONE ~/p16/R16/N10_PREFIX_DONE 2>&1"
```
If `RUN16_DONE` is missing, check `~/p16/R16/log/run16.log` (tail) for the `*_DONE` markers present and the
last `say` line to see which step it is on or stopped at; rerun with `cd ~/ntt-acc/ecalc && SLURM_JOB_ID=12287
./run16.sh` if a launch failed (idempotent — do not rerun completed steps).

All commands below: `sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com "<cmd>"`.

- **Both walls (total, elapsed)**: `cat ~/p16/R16/log/walls.txt` — one line per launch
  (`n1_record`/`n1_recheck`/`n10_record`/`n10_recheck`), each with `rc`, `total <seconds> s` (the run's own
  clock) and `elapsed <seconds> s` (wrapper, includes the disk write).
- **Phase breakdown, init**: `grep -E 'mem phase|^(init|bs|recip|dm|dc|T1|T2) ' ~/p16/R16/log/n1_record.log
  ~/p16/R16/log/n10_record.log` and `grep -E 'RESULT ecalc' ~/p16/R16/log/n1_record.log
  ~/p16/R16/log/n10_record.log` (the per-phase `RESULT ecalc <phase> s <seconds>` lines `ECALC_VERBOSE=2`
  prints).
- **comm-mark lines** (10-node run only): `grep 'comm-mark' ~/p16/R16/log/n10_record.log`.
- **Peak memory per node vs the model**: `grep -E 'mem \[end\]|VmHWM|^mem (init|bs|recip|dm|end)'
  ~/p16/R16/log/n1_record.log ~/p16/R16/log/n10_record.log` — compare the `device`/`host` totals against
  §3's layout figures (420.33 GB / 419.59 GB modelled node totals).
- **VERIFY / RECHECK lines**: `grep -E 'VERIFY OK|VERIFY FAILED|RECHECK OK|RECHECK FAILED|all [0-9]+ nodes'
  ~/p16/R16/log/n1_record.log ~/p16/R16/log/n1_recheck.log ~/p16/R16/log/n10_record.log
  ~/p16/R16/log/n10_recheck.log`.
- **Prefix verdict**: `grep -E 'the 1e11 prefix' ~/p16/R16/log/run16.log` (says "identical to" or "DIFFERS
  from" for both n1 and n10).
- **Clocks**: `grep 'aac7env: node' ~/p16/R16/log/n1_record.log ~/p16/R16/log/n10_record.log` (per-APU
  sclk/mclk/fclk at start, and periodic samples if the wall exceeded a few minutes — `ECALC_LOG_CLOCKS=1` with
  no explicit seconds value does not arm the periodic sampler, only the one-shot log at launch, since the
  driver did not set an explicit `>= 2` seconds value — confirm by checking for more than one `clocks` line
  per node in the log).
- **Disk cleanup confirmation**: `ls ~/p16/R16/n1/ ~/p16/R16/n10/ 2>&1` should show only whatever the driver
  did not delete (expect empty or only leftover files from a failed step).
- **Once all of the above is in hand**: delete this file's "## COLLECT" section and §6, and fold the numbers
  into §3/§5 with **measured** labels; add a one-line summary at the top.
