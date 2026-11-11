# target/RESTART.md — run procedure for the target system

Scope: how the target package (`probe/`, `bench/`, `ladder/`, `config/`) is run on the 576-node target: rules, decisions, settings, the steps with exact commands, known problems. Conventions and result formats: `FORMATS.md`; site facts and measured runs: `README.md`. Written 2026-10-25, revised 2026-11-10.

Status (2026-11-10): the package is in the tree and its self-checks pass (`bench/run.sh --validate`: 11 items, 0 problems; the shell scripts parse), but it has not been run on the target. The target data so far (README "Target site facts") come from hand-run sessions of 2026-10-26 and 10-29. The copy-and-paste route (`remote/`) was removed 2026-11-08 (git history, commit cfe9d3a1). A package run starts only on the user's word. The mission text, the first-five-minutes workflow, the aac7 rehearsal table and the old session route are in `archive:target_RESTART_2026-11-10.md`.

## 1. Decisions and rules

| id | decision (user, 2026-10-25 unless noted) |
|---|---|
| Q112 | raw target data is committed to `target/results/` as measured |
| Q114 | ladder sizes fixed: 1, 8, 64, 576 nodes (4 / 32 / 256 / 2304 APUs), each group on physically adjacent nodes |
| Q115 | session length unknown: order everything by priority |
| Q116 | never commit passwords, keys or tokens |
| Q119 | `MN_NONPOW2_CAP=0` on the target launch line (3.82e13 fits on 576 nodes only with it off); its time cost at non-power-of-two node counts is unmeasured (TQ81 answered the memory side only) |
| Q120 | first make every code path work on 576 nodes at 8e9 digits per node; raise the digits only after those runs succeed |
| Q121 | L3 is a staircase on 576 nodes: 4.608e12 (both paths) -> 1e13 (path 1) -> 3.82e13 (path 1), each step after the previous passed |
| Q122 | plane pools: no launch setting; the code default `ECALC_PLANE_CAP=run` picks 2^30 at the 8e9 share and 2^31 above about 9.6e9 per node (ADJUST rule 12) |

- Rules of the development cluster (queue tools, session holds, 6-node packing, NIC-loss workarounds) do not apply; their intent does: no allocated node idles, long allocations, done-markers for long waits, never touch other users' jobs, kill only by single PID.
- Correctness first: a result counts only with `VERIFY OK`. Labels m / mod / a on every number; times with the time zone; gates per `TESTING.md` before code changes.
- The coordinator keeps `target/results/LOG.md` (dated lines), `DECISIONS.md` and, at the end, `HANDOFF.md`; only it runs git (commit and push `results/` every 30-60 min and at each milestone).

## 2. Step 1 — system facts (minutes)

Read-only, every call capped at 10 s (login part <= 2 min, node part <= 5 min). Facts go to `facts.tsv` (key, value, source; keys listed at the top of `probe.sh`), full output to `raw/`, the scontrol dump of all nodes to `raw/scontrol_nodes.txt`.

```bash
L=target/results/probe/login-$(date +%Y%m%d-%H%M); N=target/results/probe/node-$(date +%Y%m%d-%H%M)
target/probe/probe.sh --only login --out "$L"
salloc -N1 --no-shell -p <partition> -t 30                 # or an sbatch hold; note the job id J
target/probe/probe.sh --only node --jobid J --out "$N"     # PROBE_SRUN_EXTRA="--gpus-per-node=4" if the APUs are hidden (dropped where the nodes list no GRES)
target/probe/gen_conf.py "$L" "$N" -o target/config/target.conf     # the login and the node probe dirs
python3 target/ladder/locality.py nodes --from "$L/raw/scontrol_nodes.txt" --save
```

Check at once: `gpu.count` = 4, `net.cxi_count` = 8, `net.nics_per_apu` = 2, `shmem.rc.*` = 0, `uffd.*` (`FI_MR_CACHE_MONITOR=userfaultfd` needs it), `lib.gmp_h` (GMP is needed only by unit tests and `ref/gen_e`; else build it into a folder and set `GMP_HOME`), and the partition limits `slurm.part.*` / `slurm.qos` / `slurm.assoc` (they size the worker team). `gen_conf.py` prints the keys it could not fill: fill them by hand with a `# how determined` comment. Commit the probe results and `target.conf` at once (Q112).

## 3. Step 2 — build

Both paths build on the login node in under a minute each (aac7: 24 s and 20 s, m) as bench items `bld01` (path 1) and `bld02` (path 2) into `target/build/<sha7>-p1|p2/` (git-ignored copies of `ecalc/` and `tools/`):

```bash
target/bench/run.sh --jobid none --only bld01,bld02      # login-node worker: runs only nodes=0 items
cat target/results/bench/bld0?/summary.txt               # status=PASS, e.binary=build/<sha7>-pN/ecalc/ecalc
```

Modules come from `target.conf` (`MNRUN_MODULES`, `MNRUN_UNLOAD`; path 2 needs the Cray SHMEM 12.1 and dsmml modules, built with `XSHMEM=cray`). The three site fixes found by the 10-26 runs (PMI library directory, SHMEM 12.1 prefix, no `--gpus-per-node` where the nodes list no GRES) and the login `SLURM_HINT` are handled in the code (`ecalc/Makefile`, `ecalc/gres.sh`, `mnrun.sh`, `BS_SEED_THREADS=auto`). If path 2 fails to build, carry on with path 1 (items and ladder accept a missing path) and fix path 2 in parallel.

## 4. Step 3 — the bench (parallel workers)

`bench/QUESTIONS.md` is the question register. The manifest holds 11 items (`bench/manifest.d/*.tsv`; contract `FORMATS.md` §4-§5): builds `bld01`, `bld02`; critical `mem01`, `cor01`, `net02`, `net04` (8 nodes), `net05` (64 nodes); write benchmarks `io01`-`io04`. Each runs once; arms of an A/B run at the same time on disjoint hosts of the allocation.

```bash
target/bench/run.sh --validate                    # manifest check
target/bench/run.sh --plan 8                      # worker sizes that drain the list fastest
mkdir -p target/results/bench/_slurm              # sbatch logs, committed (Q112)
sbatch -N <n> -p <partition> -t <min> --exclusive --gpus-per-node=4 -o target/results/bench/_slurm/%j.out \
  --wrap "cd $PWD && target/bench/run.sh --jobid \$SLURM_JOB_ID --worker w<k>"     # drop --gpus-per-node where the nodes list no GRES
target/bench/run.sh --status
```

One worker = one allocation (never two workers in one job); the runner gives each item the first hosts of the allocation and waits up to `--wait-min` (default 30) for dependencies. Stale claim: `run.sh --reclaim <id>`; redo a FAIL after a fix: `--reclaim <id> --force` (old results move to `results/bench/_old/`). A failed path-2 build only makes path-2 arms SKIP. Read the `q.*` lines of the summaries: band 0-99 answers can change the ladder settings (`ladder/ADJUST.md`).

Register rows whose item script is not in the tree (archived 2026-11-03) need a new item or a hand run. TQ84, after L0: the path 2 digits with `DIST_CHUNKS=1,2,4` at 3 nodes (`SLURM_JOB_ID=J mnrun.sh 3 env ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 DIST_CHUNKS=K ./ecalc 1e10`, `VERIFY OK`, equal digit hash with a path 1 run; `tools/gate.sh` is the aac7 form).

## 5. Step 4 — the ladder (1, 8, 64, 576 nodes)

Digits are 8e9 per node (Q120): at that share every shape fits at every level (layout D and P on path 1, path 2); at the 3.82e13 share (6.632e10 per node) P and path 2 are refused from 8 nodes on (mod, planner 2026-10-25). Settings: `ladder/levels/L<n>.conf`; model walls and the gate: `ladder/ADJUST.md`.

| level | nodes (APUs) | digits | replicas, runs | paths | write | worst plan (path 2, mod) |
|---|---|---|---|---|---|---|
| L0 | 1 (4) | 8e9 | 8, 2 | both | no | device 144 GB, node 191 GB |
| L1 | 8 (32) | 6.4e10 | 4, 2 | both | no | device 216 GB, node 293 GB |
| L2 | 64 (256) | 5.12e11 | 2, 2 | both | first replica | device 216 GB, node 293 GB |
| L3 s1 | 576 (2304) | 4.608e12 | 1, 2 | both | yes | device 328.69 GB (+44.75), node 406.32 GB |
| L3 s2 | 576 | 1e13 (1.74e10 per node) | 1 | path 1 (path 2 refused) | no | D device 238.97 GB (+134.47) |
| L3 s3 | 576 | 3.82e13 (6.632e10 per node) | 1 | path 1 | no | D device 358.96 GB (+14.48), node 395.04 GB |

```bash
target/ladder/ladder.sh --level L3 --run-tag s1
target/ladder/ladder.sh --level L3 --run-tag s2 --set DIGITS=10000000000000 --set WRITE=0 --set MODEL_NOWRITE_S=52.3 --set MODEL_WRITE_S=55.3
target/ladder/ladder.sh --level L3 --run-tag s3 --set DIGITS=38200000000000 --set PATH=1 --set WRITE=0 --set MODEL_NOWRITE_S=187.1 --set MODEL_WRITE_S=191.2
```

A failing step stops the staircase: record it, apply ADJUST.md, do not move up. Add `--set WRITE=1` to s3 only if the file system has room for 17 TB. Run 1 of a fresh allocation is a cold start: every level does 2 runs and is timed from run 2. `ladder.sh` drops a path whose plan is refused.

```bash
python3 target/ladder/locality.py nodes --save          # refresh the physical table with the current node states
target/ladder/ladder.sh --level L0 --dry-run            # plan check, groups, job lines
target/ladder/ladder.sh --level L0                      # 8 replicas, one sbatch each, adjacent groups
target/ladder/ladder.sh --collect L0                    # after the DONE markers: mean/min/max, spread, VERIFY, hash agreement
#   then L1, L2, L3; inside an allocation you hold: --jobid J; overrides: --set KEY=VALUE (e.g. --set ECALC_NP=4)
```

Each replica writes `results/ladder/<L>/<tag>-r<k>/{summary.txt,raw/,DONE}`; nothing waits. L0-L2 can run while bench workers use other nodes; L3 needs the whole machine.

## 6. Order of work and the team

Priority order (Q115): step 1 -> step 2 -> in parallel bench items with prio < 100 and L0 (once both builds pass) -> bench prio 100-199 with L1/L2 replicas on other nodes -> L3 -> bench prio 200+ in whatever nodes are free. Workers (subagents or background shells) each hold one allocation and run `bench/run.sh` or one ladder level; they never ask the user. Ask for long allocations and run many items inside them (fewer job starts and reboots).

## 7. Settings, limits, paths

- Launch settings (`config/target.conf.template` -> `target.conf`): `ECALC_NP=auto`, `MN_T_CHUNK_MB=2048`, `MN_NONPOW2_CAP=0`, `FI_MR_CACHE_MONITOR=userfaultfd`, `FI_LOG_LEVEL=warn`, `ECALC_DEVICE_GB=373.44`, `ECALC_MEM_GUARD_GB=6`, `ECALC_SEGV_TRACE=1`, `ECALC_VERBOSE=2`, `MEM_REPORT_DEVS=1`. Pools, heaps and `FI_UNIVERSE_SIZE` are computed by the planner and `mnrun.sh`, never typed. Bad nodes are dropped (`MNRUN_EXCLUDE_HOSTS`, mode `drop`), never replaced by spares.
- Memory: device <= 373.44 GB per node (the `hipMalloc` edge, 93.36 GB per APU, m); host <= `ECALC_NODE_GB` (487 from MemAvailable 515.7 GB idle, m; code default 480). The planner (`MN_PLAN_ONLY=<digits>:<nodes>`, `tools/plan_values.py --fit`) is the authority; at 3.82e13 on 576 nodes the device need is 358.96 GB only with `MN_NONPOW2_CAP=0` (407.41 GB with the default: refused; mod). Largest size: about 4.19e13 with `MN_NONPOW2_CAP=0`, about 2.67e13 with it on (mod).
- Two multi-node paths (Q85, digits identical): path 1 libfabric (default build `make -C ecalc GMP_HOME=<gmp>`; layout D: `SLURM_JOB_ID=<J> ecalc/mnrun.sh <nodes> env ./ecalc <digits>`); path 2 Cray SHMEM (`make -B -C ecalc XSHMEM=cray GMP_HOME=<gmp>` with the Cray SHMEM and dsmml modules; one process per GPU: `mnrun.sh <nodes> env ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ./ecalc <digits>`). `ECALC_PE_PER_GPU=1` on the default build is a third shape (P).
- Still to bring back from the target: fabric injection per APU (1 and 2 NICs), all-to-all scaling with node count, latency, Cray SHMEM 12.1 start-up cost, ROCm 7.14 speed, the writer's file-system rate, node-to-node variation, queue rules. Record raw output; post-access development uses only `target/results/`.

## 8. Known problems and fixes

| symptom | cause and fix |
|---|---|
| hang in memory registration (ROCr <-> libfabric) | keep `FI_MR_CACHE_MONITOR=userfaultfd` (the `mnrun.sh` default; 0/104 hangs vs 22/105 with memhooks, m aac7); `disabled` gives wrong exchanges and is refused. If the kernel refuses userfaultfd (probe `uffd.*`), stop and tell the user before any multi-node run |
| rc 4 from `mnrun.sh` / `ladder.sh` | the memory plan does not fit (`ECALC_DEVICE_GB` / `ECALC_NODE_GB`): lower the digits (the refusal prints the largest that fits). rc 8 = `ECALC_BUDGET_CHECK` stop, rc 6 = allocation failed |
| rc 9 during a run | the memory guard (`ECALC_MEM_GUARD_GB=6`) stopped the run before the OOM killer: lower `ECALC_NODE_GB` by the printed overshoot |
| rc 134 | `abort()`; with `ECALC_SEGV_TRACE=1` the trace is in the log; keep it in `results/` |
| `fi_enable(-28)` with several PEs per node | launch Slurm-native (`srun --ntasks`), as `mnrun.sh` does |
| a task gets 1-2 CPUs; init 150 s instead of 13-16 s | `srun` inherits `SLURM_HINT=nomultithread` and `SLURM_DISTRIBUTION=block:block` from the login environment and OpenMP latches 1-2 threads. `mnrun.sh` drops the hint (`MNRUN_KEEP_HINT=1` keeps it), passes `--distribution=block` and defaults `MNRUN_CPUS_PER_TASK=auto`; `BS_SEED_THREADS=auto` protects hand launches, which need `--hint=multithread` or `-c <CPUs per task>` |
| link or load errors after `module load rocm` | a versioned ROCm conflicts with the login default: `MNRUN_UNLOAD` in `target.conf` (the probe fills it) |
| APUs invisible inside a job step | `--gpus-per-node=4` in the job and `--gpu-bind=none` on the step, only where `scontrol show node` lists a gpu GRES (`MNRUN_GRES=auto|on|off`; a site with `Gres=(null)` rejects them) |
| `locality: no node with an xname` / ladder `no group` | no HPE xname in `scontrol show node`: write a host-to-xname map (`host xname` per line), `locality.py nodes --map MAP --save`, then `ladder.sh ... --nodes-from target/results/probe/topology.tsv` (re-save before each level) |
| ladder `plan check of path N FAILED (rc 2, not a memory refusal)` | the planner could not run (module, binary, `target.conf`); the named plan file has the first error. A memory refusal is rc 1: that path is dropped |
| one node slow (replica 5-10 % behind) or a NIC missing | `tools/nodescreen.sh` (8 NICs per node); add the host to `results/ladder/exclude.txt`; no spares |
| NICs lost after jobs end (aac7) | long allocations; check `fi_info -p cxi` counts at the start of each allocation |
| first run on a fresh allocation slow | cold start (aac7 154.9 s vs 76.9-85.5 s, m): time from run 2 (ADJUST rule 11). aac7 is also bimodal (one run in three 5-8 % slower): measure arms at the same time on different nodes |
| `ECALC_VERBOSE=2` very long at 576 nodes | use `ECALC_VERBOSE=1` (the L3 conf does) |
| `ECALC_NP=3` | one node only; at more than one node an explicit 3 is refused (use auto or 4) |
| random multi-second stalls of all APU threads of a node | kernel compaction of a 4 KiB pinned SHMEM heap (path 2): `profiles/README.md` and `results/TRANSPORT.md` |
| a run seems hung but is a slow file system | no digit output in timing runs; wait for `total` |
| start-up hang (1 in 31 at 7.7e10 on aac7, 0 in 301 since) | kill only that job by its id, rerun once, record it in `LOG.md` |
| Cray SHMEM 12.1 (path 2) start-up slow or failing | path 1 is complete on its own; record the failure (probe `shmem.*`, register row TQ11) |

## 9. Before access ends

Commit and push `target/results/` (summaries under `results/bench/` included); write `target/results/HANDOFF.md`: what ran, what is left by priority, surprises, next questions.
