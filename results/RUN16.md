# RUN16 — today's optimized configuration at near-limit sizes on aac7, 1 node then 10 nodes

**Both runs PASS, measured.** 1 node, 1.03 × 10¹¹ digits: `total` 193.21 s, VERIFY OK, RECHECK OK, the 10¹¹
prefix identical to the reference, device peak 387.2 GB + host HWM 27.7 GB ≈ 415 GB/node (model: 420.33 GB).
10 nodes, 8.1 × 10¹¹ digits total (try 2, after a try‑1 timeout — §"10-node part, try 2"): `total` 3532.05 s,
VERIFY OK on all 10 nodes, RECHECK OK on all 10 nodes, the 10¹¹ prefix identical, device peak 378.6 GB + host
HWM 36.8 GB ≈ 415 GB/node (model: 419.59 GB). Times Eastern; aac7 is PDT = Eastern − 3 h.

## 1. Commit, build, modules

- Repo `~/ntt-acc` on aac7 refreshed from a stale clone (e5466b7) to **origin/main 5ae3278** via a GitHub
  HTTPS fetch (aac7 cannot reach GitHub by SSH key for `git@github.com:...`, but
  `https://github.com/cecoppola/ntt.git` works): `git fetch https://github.com/cecoppola/ntt.git
  main:refs/remotes/gh/main && git merge --ff-only gh/main`. Commit 5ae3278 = "internal target wishlist v2:
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

**10 nodes** (verbatim, the task's launch line = internal target notes §4 adapted to aac7 by `e16_headline.sh`):
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

## 6. Results (measured, collected from `~/p16/R16/log/`)

**Walls** (`~/p16/R16/log/walls.txt`): `n1_record` rc 0, `total` 193.21 s, elapsed 570 s; `n1_recheck` rc 0,
elapsed 432 s; `n10_record` try 1 rc 124 (killed at its 3600 s timeout, elapsed 3602 s — see "10-node part,
try 2" below); `n10_record` try 2 rc 0, `total` 3532.05 s, elapsed 3541 s; `n10_recheck` rc 0, elapsed 1751 s.

**Phase breakdown** (`RESULT ecalc <phase> s <seconds>`, `ECALC_VERBOSE=2`):
- n1: init 22.57, bs 93.49, 10dP 0.18, dm 76.75, T1 0.0003, T2 0, dc 373.11 (cumulative dc marker, not a
  separate wall component — `total` 193.21 = bs + 10dP + dm + T1 + dc(0) + T2 = 170.42, + init 22.57 + other
  0.22), vmhwm 27.66 GB.
- n10 (max over the 10 ranks; init varies 26.85–131.08 s across ranks, likely the Cray `shmem_init_thread`
  stagger): init 128.72–131.08 s, bs 1225.33 s, 10dP 0 s, dm 2174.15 s, `total` 3532.05 = (bs 1225.3 + 10dP 0 +
  dm 2174.2 + T1 0.6 + dc 0.1 + T2 0 = 3400.2) + init 128.8 + other 3.07, vmhwm 36.83 GB.

**comm-mark** (10-node run, `MN_COMM_MARK=1`, per APU thread, summed over the 4 APU threads × 10 nodes):
tree level 1: 1380 exchanges, 599.31 GB, 1.05 GB/s, wall 324.06 s; tree level 2: 4904 exchanges, 2779.33 GB,
0.75 GB/s, wall 1023.72 s; reciprocal: 6788 exchanges, 1209.80 GB, 1.38 GB/s, wall 267.65 s; division (after
the reciprocal): 8404 exchanges, 4421.32 GB, 0.61 GB/s, wall 1906.51 s — the division's exchange dominates the
wall (54 % of `total`), at under half the tree level 1 rate (fabric contention grows with more concurrent
peers, consistent with the per-level rate falling 1.05 → 0.75 → (recip 1.38, an outlier, fewer/larger
messages) → 0.61 GB/s).

**Peak memory per node vs the §3 model** (device/host at the `bs` phase boundary, the peak for both runs; host
figure is VmRSS at that boundary, not VmHWM, since the two differ by ≤ 0.1 GB here):
- n1: device 387.2 GB (flat across bs/recip/dm — the arena is allocated once and does not shrink until `end`)
  + host 28.8 (bs)/7.4–8.0 (recip/dm) GB ≈ **≈ 415–416 GB/node measured** against **420.33 GB modelled** (§3):
  model 4–5 GB over measured, consistent with mem_model's usual small conservative margin.
- n10: device 378.6 GB (rank 0, the `bs` phase peak; `end` falls to 103.7 GB once planes are freed) + host 28.8
  GB RSS ≈ **≈ 407 GB/node measured** against **419.59 GB modelled** (§3): model ≈ 12 GB over measured.

**VERIFY / RECHECK**: n1 `VERIFY OK`; n1 `RECHECK OK`. n10 `mn: node 0..9: VERIFY OK` on every node, `mn: all
10 nodes: VERIFY OK`; `mn: node 0..9: RECHECK OK` on every node, `mn: all 10 nodes: RECHECK OK`.

**Prefix verdict**: both `n1: the 1e11 prefix is identical to .../ntt/ecalc/results/e_1e11.out` and `n10: the
1e11 prefix is identical to .../ntt/ecalc/results/e_1e11.out`.

**Clocks**: 7 `aac7env: node` lines per node in both logs (task/cpus, 4× GPU clocks, the APU→NUMA→cxi map, the
HIP library path) — one-shot only, confirming `ECALC_LOG_CLOCKS=1` with no explicit seconds value does not arm
a periodic sampler. n1 clocks (x9000c1s0b0n0): GPU0 fclk 2000 mclk 1300 sclk 95, GPU1/2 fclk 1200 mclk 900
sclk 94, GPU3 fclk 2000 mclk 1300 sclk 94; `apus 0000:02:00.0:numa0 … nics cxi0:numa0 … cxi3:numa3` (one NIC
per APU, matching NUMA node).

**Disk cleanup**: `~/p16/R16/n1/` and `~/p16/R16/n10/` both empty after the run — confirmed, nothing of either
run's digits left on disk.

## 10-node part, try 2 (run16b.sh)

- Try 1 (run16.sh, n10_record): rc 124, killed by the driver's own 3600 s timeout at elapsed 3602 s. Log shows
  the reciprocal exchange at 0.54 GB/s per APU thread (6788 exchanges, 1209.80 GB received, 2221.48 s post to
  completion summed over APU threads; wall 701.29 s) vs 1.49 GB/s in the earlier 8.2e11 run that completed in
  2353 s total. No n10/e.out.part* files existed yet (killed before the write phase), so nothing needed cleanup;
  log renamed to ~/p16/R16/log/n10_record_try1.log.
- Try 2 (run16b.sh): launched 2026-10-06 00:38 EDT (2026-10-05 21:38 PDT on aac7) inside job 12287, PID 2015444
  (parent setsid/nohup PID 2015443). Skips the already-done 1-node part (N1_PREFIX_DONE) and the already-passed
  n10 plan check (N10_PLAN_DONE), and reruns record -> RECHECK -> partial unpack -> prefix compare only, same
  env/size (8.1e11 digits, 10 nodes, D10_PER=81000000000). Record and RECHECK timeouts both raised to 9000 s
  (~3.8x the measured try-1 elapsed). Always touches ~/p16/R16/RUN16_DONE at the end (success or failure),
  status recorded in ~/p16/R16/log/walls.txt, since ~/nic16/mn.sh waits on that marker.

- Try 2 result: SUCCESS. n10_record rc 0, total 3532.05 s / elapsed 3541 s (well inside the 9000 s budget);
  n10_recheck rc 0, elapsed 1751 s ("mn: all 10 nodes: RECHECK OK"); 1e11-digit prefix identical to
  ~/ntt/ecalc/results/e_1e11.out. RUN16_DONE touched at 2026-10-05 23:47 PDT (2026-10-06 02:47 EDT), unblocking
  the waiting ~/nic16/mn.sh. n10 part files deleted after the prefix check, logs kept, per run16b.sh.
