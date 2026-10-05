# ACC16 — deferred 1-node acceptance of main (R + S merged) on aac7, ROCm 7.2.4 (Phase 16 ACC)

Branch `p16-ACC` from `main` 8a56331 (= 1be410f + one docs-only commit, `docs/TARGET_WISHLIST.md`; no code
changed, so the run below is equally valid against 8a56331). Times Eastern (aac7 is PDT = Eastern − 3 h).
This closes the open item left by R16 §"Open issues" and S16 §5 ("the unit / e9 rerun on a 1-node job when
nodes free up") — both landed in `main` at 1be410f (S's `COMM_INIT_EARLY` switch, `mnaccept.sh`'s `-c` fix;
R's `BS_POOL_RULE`).

## 1. Setup

Fresh clone at `~/ntt-acc` on aac7 (bundle of `origin/main` 1be410f, since aac7 cannot reach GitHub by SSH
key — `git ls-remote git@github.com:cecoppola/ntt.git` gets `Permission denied (publickey)`). Built with
`source ecalc/aac7env.sh && cd ecalc && make SHMEM_CRAY=1 GMP_HOME=~/gmp` (rocm/7.2.4, the aac7 default
since V16): clean build, `ecalc` and every `tests/t_*` including `t_mn_grid` produced.

## 2. The job: three resubmissions (12220 → 12226 → 12235)

Submitted `sbatch -p 192C4G1H_MI300A_RHEL9_A1 -N1 --gpus=4 -t 1:30:00 -J ACC16 --wrap "sleep 5400"` per
`ecalc/README.md` line 175 (job 12218 first, no `--gpus`, immediately hit `srun: ... Invalid generic
resource (gres) specification` — cancelled, resubmitted with `--gpus=4`).

- **12220** (x9000c1s4b0n0, 20:42–22:12 PDT = 23:42–01:12 EDT): ran into the real bug below; its first four
  unit steps each spent their full 1200 s `timeout` retrying `srun: ... Requested nodes are busy` and
  failed (rc 124); the job then hit its own 1:30 limit (Slurm `TIMEOUT`) before `e9` ran.
- **12226** (x9000c1s6b0n0, 22:58–23:21 PDT): same symptom confirmed on a node that `sinfo` showed fully
  `idle` beforehand, ruling out leftover contention from D16E (which the account's own other session had
  already cancelled at 20:14 PDT, unprompted by me) or from `bobrobey`'s concurrent `cdash_testing` jobs
  (different nodes). Killed by exact PID (`1372966`/`1372967`, the stuck `timeout`/`srun` of the `t_mul`
  step) once diagnosed, then the job cancelled — no other user's job was touched.
- **Root cause** (`scontrol show job`): `sbatch -N1 --gpus=4` with no `-c`/`--cpus-per-task` grants the job
  only **`NumCPUs=1`** on this partition (aac7's 192-core nodes are not given to a job by core count unless
  asked). `mnaccept.sh`'s `R()` then runs `srun --jobid=$J -N1 --gpus=4 -c 192 --overlap …` (`-c 192` from
  `MNRUN_CPUS_PER_TASK=auto` → `scontrol show node` CPUTot) — 192 CPUs out of a 1-CPU allocation, which
  Slurm can never grant, hence the endless "busy" retries until the step's own `timeout` fires. `--exclusive`
  alone (tried once, job 12234) left the request pending on priority rather than fixing `NumCPUs`; the
  working fix is **`-c 192`** on the `sbatch` line.
- **12235** (x9000c1s1b1n0, from 23:22:26 PDT = 02:22:26 EDT): `sbatch … -N1 -c 192 --gpus=4 -t 1:30:00`
  (`AllocTRES=cpu=192,…,gres/gpu=4`) — every step passed, no retries. Cancelled after both runs (§3), a job
  of my own; D16E/D16/P16/skoranne's interactive jobs were never touched.

**Trap for the integrator / README**: on aac7 (unlike wherever the `sbatch -N1 --gpus=4` line in
`ecalc/README.md` §"The standing regression" was last run), `mnaccept.sh`'s holding job needs an explicit
`-c <cpus>` (or equivalent) matching `MNRUN_CPUS_PER_TASK`/the node's core count — `--gpus=N` alone does not
reserve the node's CPUs on this partition, and the symptom (`srun: Requested nodes are busy`, retried for
the full per-step timeout) gives no hint that the job itself is CPU-starved.

## 3. The two mnaccept runs on job 12235 (`COMM_INIT_EARLY=1`, the S16 fix)

**Run 1 — `BS_POOL_RULE=1` (default, R16's fix) — `./mnaccept.sh 12235 --only unit,e9`, 23:22–23:40 PDT
(02:22–02:40 EDT):** **11 passed, 0 failed, 1047 s.**

| step | result |
|---|---|
| `t_ntt 24` | VERIFY OK (3215 checks) |
| `t_mul 20` | VERIFY OK (189 checks) |
| `t_bs` | VERIFY OK (10 checks) |
| `t_dbig 0` | VERIFY OK (555 checks) |
| `t_newton 20` | VERIFY OK (620 checks) |
| `t_verify` | VERIFY OK (334 checks) |
| `t_out` | VERIFY OK (1 check) |
| `t_patch` | VERIFY OK (both output forms) |
| **`t_mn_grid 1 28` (2 node-processes, 1 node)** | **VERIFY OK (200 checks), both PEs** — the S16 open item (TCP `t_mn_grid` timeout was a **2-node** artifact; on 1 node it passes directly, no `COMM_TRANSPORT` override needed) |
| `e9` size 1, `LIMB_BASE=10` | identical to `ref/e_1000000000.txt`; total 13.48 s |
| `e9` size 1, `LIMB_BASE=2` | identical; total 23.30 s |

**Run 2 — `BS_POOL_RULE=0` (the old, pre-R16 sizing rule) — `./mnaccept.sh 12235 --only e9`, 23:48–23:50 PDT
(02:48–02:50 EDT):** **2 passed, 0 failed, 107 s.**

| step | result |
|---|---|
| `e9` size 1, `LIMB_BASE=10` | identical; total 11.32 s |
| `e9` size 1, `LIMB_BASE=2` | identical; total 24.37 s |

**`BS_POOL_RULE=1` vs `=0` at 10⁹ on 1 node: digits identical both bases** (`BS_POOL_RULE` is a sizing-pass
correction only — R16 §2 — and 10⁹:1 is far from the boundary levels either rule affects, so this is the
expected confirmation, not a new result).

No reference-file gap: `~/ntt/ecalc/ref/e_1000000000.txt` (and `e_100000000.txt`, unused by `--only unit,e9`)
were present and auto-symlinked into the clone's `ref/` by `mnaccept.sh`.

## 4. Summary for the integrator

- **main at 1be410f (= HEAD 8a56331, docs-only since) passes the deferred 1-node acceptance**: all 9 unit
  steps including `t_mn_grid` on 1 node, and `e9` at 10⁹ in both limb bases, under the merged R (`BS_POOL_RULE`)
  and S (`COMM_INIT_EARLY`, the `mnaccept.sh` `-c` fix) changes. `BS_POOL_RULE=1`/`=0` give identical digits
  at 10⁹:1, as expected.
- **New trap, not in either R16 or S16**: `mnaccept.sh`'s holding `sbatch` needs `-c <node core count>`
  (e.g. `-c 192` on aac7) in addition to `--gpus=N`, or every `unit` step burns its full timeout retrying
  `srun: Requested nodes are busy` and the job can time out before `e9` runs. Worth a line in
  `ecalc/README.md` §"The standing regression" next to its `sbatch … --gpus=4 …` example.
- Not run here (out of scope for this acceptance): `mn`, `ckpt`, `recheck`, `corr`, `--full`.

## RESUME
- 2026-10-05 02:51 EDT: **done.** Job 12235 cancelled, no job of mine left on aac7 (D16/D16E/P16/skoranne's
  untouched throughout). `~/ntt-acc` (clone, built) and `~/ntt-main.bundle` left on aac7 for reuse. Nothing
  pending.
