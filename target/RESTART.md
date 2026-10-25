# target/RESTART.md — start here on the target system

Scope: everything an agent needs to start work on the target system at once: the mission, the rules and how to install
them, the project as we understand it, and steps 1-4 with exact commands. Written 2026-10-25 (Eastern time) by the
coordinator on the development side, which has no access to the target. Conventions: `target/FORMATS.md`.

## 0. First five minutes

1. `git clone` this repository (or `git pull` the existing clone) on the target login node; `cd` into it.
   `APU=$(dirname "$PWD")` — the work root (the parent of the clone; it plays the role of the development side's work root).
2. Install the rules and memory (§2). Start the coordinator session (Claude Code) in the clone.
3. Read this file to the end, then `target/FORMATS.md`, then `target/README.md`.
4. Start a log: `target/results/LOG.md` (dated lines, times with the time zone).
5. Run step 1 (§5) now; it takes minutes and everything else depends on it.

## 1. Mission (the user's request of 2026-10-25, verbatim)

> (1) A new folder in the repository specifically built for all our target remote testing and evaulation. This will
> contain information we put into it now, and it will also store information we collect during the tests that we will
> use later. The remaining work items all go into this folder.
> (2) A set of quick system tests to collect all of the critical information about the target system. This includes all
> filepaths, libraries, version numbers, hardware information, network architecture, queue system and rules, and
> everything else you will need to work. You will use this data to produce the target-specific machine configuration.
> These must all be fast callsw, and you can write a script and/or instructions for them.
> (3) A set of short and medium-length benchmarks, parameter sweeps, and tests, which you will use to answer every
> outstanding question we have about how to optimize the algorithm for the target system. This must be designed to run
> on a team of many agents working in parallel to collect the information without bottlenecking any tests. These must
> be designed to be fast, not exhaustive, so do not run them multiple times and write them to be efficient. These must
> also measure every piece of information you could possibly need in order to continue doing testing and development on
> this system, after the target access is over, and we are doing development without direct access to the system.
> (4) A nearly-complete version of ecalc that you will have ready to run on the system after (2) & (3). You will design
> this to run on 1 node, 1 chassis, 1 rack, and then the entire system. For the HPE cabinets we are dealing with, I
> believe that is 4 APUs, 32 APUs, 256 APUs, and 2304 APUs. I want you to figure out how to index the nodes based on
> their physical location and run the groups according to their physical locality. Be prepared to run these in order,
> where you run it a few times at the smallest level, measure the results, make adjustments, and then move to the next
> size level. These must be ready to run quickly, in order. For the lower 3 levels, you can run multiple identical jobs
> in different queues since there will be many nodes available.
> (5) You will also need to write a detailed, fully complete RESTART.md file that contains every piece of information
> that an agent will need to immediately spring into action on the target system, including these instructions (1),
> (2), (3), and (4). This file should contain your understanding of the project and your context for continuing this
> work there. You also need to copy this system's claude.md and other governing settings files into this folder and
> instruct the agent, in the RESTART.md file, to copy all of those to the correct locations on the target system.

The user's decisions on this project (2026-10-25):

| id | decision |
|---|---|
| Q112 | raw target data is committed to `target/results/` as measured (the no-identifying-data rule is relaxed for this folder) |
| Q113 | the governing files are copied verbatim into `target/governance/` |
| Q114 | ladder sizes fixed: 1, 8, 64, 576 nodes (4 / 32 / 256 / 2304 APUs), each group on physically adjacent nodes |
| Q115 | session length unknown: order everything by priority, so any stopping point leaves the most important answers |
| Q116 | ignore the leak guard for target work; never post passwords, keys or tokens under any circumstances |
| Q117 | `docs/` is untracked and stays so; this file carries what the target agent needs from it |
| Q118 | the governance copies are exact; the user overrode the development side's commit guard for them ("My instructions supersede everything else") |

## 2. Install the rules (do this first)

`target/governance/` holds the development side's rule files. Install them with
`bash target/governance/install.sh` from the clone's root (existing files are kept and the copy is put next to them as
`<name>.from-dev`; `--force` overwrites). It places:

| file in `target/governance/` | installed as |
|---|---|
| `apucode_CLAUDE.md` | `$APU/CLAUDE.md` (project rules; Claude Code reads it for every session under `$APU`) |
| `home_claude/CLAUDE.md` | `~/.claude/CLAUDE.md` (the user's global agent rules) |
| `home_claude/COORDINATOR.md` | `~/.claude/COORDINATOR.md` (injected into the main session by a hook) |
| `home_claude/settings.json` | `~/.claude/settings.json` (hooks, model settings, env; merge by hand if the target has its own) |
| `home_claude/hooks/*` | `~/.claude/hooks/` (executable) |
| `home_claude/agents/*.md` | `~/.claude/agents/` (the four agent types: haiku-scout, haiku-worker, sonnet-developer, sonnet-reviewer) |
| `memory/*.md` | `~/.claude/projects/<key>/memory/`, `<key>` = the clone's absolute path with every `/` replaced by `-` |
| `project_claude/settings.local.json` | `<clone>/.claude/settings.local.json` |

The copies are exact. They name the development side's work root (`/home/machinus/apucode`); the script rewrites it to
the target's work root `$APU` in the installed project rules and memory.
Restart Claude Code after installing so the hooks load.

How the rules apply on the target (they were written for the development side):
- **Target rules win** where they differ: this file and the Q112-Q118 decisions above.
- Rules about the development cluster `aac7` (its queue tool `aac7q`, `qadd`, `bin/c7`, `bin/cp7`, session holds,
  the 6-node packing limit, the NIC-loss workarounds, the password file) do not apply on the target; their intent does:
  pack work so no allocated node idles, use long allocations and run many items inside them, drivers with done-markers
  for long waits, never touch other users' jobs, kill only by single PID.
- The private `docs/` files (STATUS, DECISIONS, AGENT_PROTOCOL) do not exist in the clone (Q117). On the target the
  coordinator keeps its live state in `target/results/LOG.md` and decisions in `target/results/DECISIONS.md` (create both).
- The local leak-guard git hooks are not part of the clone; on the target nothing blocks a commit, so the coordinator
  itself makes sure no password, key or token is ever committed (Q116). Raw data (host names, paths, versions) is fine (Q112).
- Correctness first, labels m / mod / a, gates before merging code changes (`TESTING.md`), priorities, parallel work:
  all apply unchanged.

## 3. The project as we understand it

**What ecalc is.** `ecalc` computes ⌊10^d · e⌋ in decimal: binary splitting of Σ 1/k!, a Newton reciprocal and a
two-half division, on numbers held in device memory as base-10¹⁸ limbs (no radix conversion). Products are
number-theoretic transforms modulo three or four primes (`ECALC_NP=auto`), distributed over the four APUs of a node and
over node groups (a tree of group sizes, `MN_GROUPS`, computed by the planner). Every run verifies itself (T1 residues,
T2 windows: `VERIFY OK` on every node); `ECALC_RECHECK=1` re-checks a finished file; `tools/unpack_digits` turns the
packed output (0.417 B per digit) into ASCII. Design: `ALGORITHM.md`; switches: `ecalc/README.md`; plan: `PLAN.md`;
how to test: `TESTING.md`; open work: `TASKS.md`; ideas: `OPPORTUNITIES.md`; evidence: `results/`.

**The goal.** First (user decision Q120): ecalc running efficiently on all 576 nodes on every code path, at a small size
(8 × 10⁹ digits per node, 4.608 × 10¹² digits on 576 nodes); then larger sizes up to the long-standing headline
d = 3.82 × 10¹³ digits on all 576 nodes (6.63 × 10¹⁰ digits per node), digits right without a
reference, as fast as possible. Standing estimate 182-185.5 s without the write (mod, fabric assumed at the 100 GB/s
injection peak per APU); the write adds 1.0 s at the file system's 1 TB/s or 12.8 s at 1.0 GB/s per node (mod).
The fabric numbers are the biggest unknown: the bench measures them first.

**Two multi-node paths (user rule Q85, both stay working; digits identical between them).**
1. Path 1, libfabric (default build `make -C ecalc GMP_HOME=<gmp>`, transport xshmem over `xshmem_ofi.c`): layout D =
   one process per node with four APU threads. Run: `SLURM_JOB_ID=<J> ecalc/mnrun.sh <nodes> env ./ecalc <digits>`.
2. Path 2, Cray SHMEM (the target's shape; `make -B -C ecalc XSHMEM=cray GMP_HOME=<gmp>` with the Cray SHMEM and dsmml
   modules): one process per GPU, two lanes: `mnrun.sh <nodes> env ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ./ecalc <digits>`.
   Never run on Cray SHMEM 12.1 before this session (aac7 has 11.8).
   `ECALC_PE_PER_GPU=1` on the default build (path 1 with one process per GPU, "P") is a third shape: fastest on aac7 at
   2 nodes where it fits, slower at 1 node.

**What aac7 measured (the development cluster: same node type, 1 NIC per APU, 6 nodes per user; all m, 2026-10-25,
`results/LAUNCH_DEFAULTS.md §AAC7OPT`):**

| size | result |
|---|---|
| 1 node, 6.6e10 (the target's per-node share) | layout D 74.2 s (fastest); P 83.8 s; `ECALC_NP=3` = auto; `ECALC_NP=4` +7.5 s; seed chunk, thread counts: noise |
| 2 nodes, 5e10 | P (path 1) 39.7 s, D 54.4 s, path 2 53.5 s; `MN_T_CHUNK_MB` 1024 vs 2048: noise |
| 3 nodes, 1e11 | path 1 81-83 s; path 2 85-93 s (+7.5 %, the whole gap is start-up, aac7-specific) |

aac7 times are bimodal (about one run in three is 5-8 % slower on both paths), so every A/B there is paired; on the
target, with many nodes, run the arms at the same time on different node groups instead (the bench does).

**Memory (the hard limit).** Device memory per node ≤ 373.44 GB (the `hipMalloc` edge measured on the target, 93.36 GB
per APU, m); host ≤ `ECALC_NODE_GB` (480 by default, set from the probe). The planner (`MN_PLAN_ONLY=<digits>:<nodes>`,
`tools/plan_values.py --fit`) is the authority; `mnrun.sh` refuses a run that does not fit (rc 8), the guard stops a run
before the OOM killer (rc 9). At 3.82e13 on 576: device 358.96 GB (+14.48 GB) only with `MN_NONPOW2_CAP=0` (user decision
Q119; with the code default 407.41 GB, refused; mod, planner). The largest size that fits: about 4.19e13 with
`MN_NONPOW2_CAP=0`, about 2.67e13 with it on (mod). Its time cost at non-power-of-two node counts is measured by the bench.

**The launch settings for the target** (`target/config/target.conf.template` → `target.conf`): `ECALC_NP=auto`,
`MN_T_CHUNK_MB=2048`, `MN_NONPOW2_CAP=0`, `FI_MR_CACHE_MONITOR=userfaultfd`, `FI_LOG_LEVEL=warn`, `ECALC_DEVICE_GB=373.44`,
`ECALC_MEM_GUARD_GB=6`, `ECALC_SEGV_TRACE=1`, `ECALC_VERBOSE=2`, `MEM_REPORT_DEVS=1`; pools, heaps and
`FI_UNIVERSE_SIZE` are computed by the planner and `mnrun.sh` (never typed). Bad nodes are excluded
(`MNRUN_EXCLUDE_HOSTS`, mode `drop`), never replaced by spares (user decision 2026-10-11).

**What the development side cannot know and the session must bring back:** the fabric (injection per APU with 1 and 2
NICs, all-to-all scaling with node count, latency, the dragonfly group size), Cray SHMEM 12.1 behaviour and start-up
cost, ROCm 7.14 speed against 7.2.4, the file-system write rate of ecalc's own writer, node-to-node variation, the queue
rules, and every model constant listed in `bench/QUESTIONS.md`. Post-access development uses only what is committed in
`target/results/`, so record raw output, not just conclusions.

## 4. How to work on the target: the team

- **Coordinator** (the Claude Code main session in the clone): plans, starts workers, collects, commits and pushes
  `target/results/` every 30-60 min and at each milestone (Q115: the session can end at any time), keeps
  `target/results/LOG.md` and `target/results/DECISIONS.md`, and talks to the user. Only the coordinator runs git.
- **Workers** (subagents, or background shells the coordinator starts): each holds one Slurm allocation and runs
  `target/bench/run.sh --jobid J` (claims items one after another) or one ladder level. Workers never ask the user and
  return once (CLAUDE.md protocol). Size the team from the queue rules in the probe and from `bench/run.sh --plan N`.
- **Long allocations, many items inside** (the aac7 lesson: fewer job starts and reboots): ask for an allocation sized for
  a block of items, keep it while it has work.
- **Order of work** (priority, Q115): step 1 (probe, conf) → step 2 (builds) → in parallel: bench items with prio < 100
  (the ladder needs them) and ladder L0 as soon as both builds pass → bench prio 100-199 (optimization questions) with
  L1 and L2 replicas on other nodes → L3 → bench prio 200+ (calibration for offline work) in whatever nodes are free.
- **Correctness first:** a result counts only with `VERIFY OK`; any wrong digit or crash stops that line of work until
  understood. Code changes on the target follow `TESTING.md` (gate before use) and go to a branch; push it.
- **Labels and times:** every number m / mod / a; times with the time zone; raw output kept under `target/results/`.

## 5. Step 1 — system facts (minutes)

Fast, read-only, every call capped at 10 s (login part ≤ 2 min, node part ≤ 5 min). Facts go to `facts.tsv` (key, value,
source; the keys are listed at the top of `probe.sh`), full command output to `raw/`, and the scontrol dump of all nodes
to `raw/scontrol_nodes.txt` (input of `ladder/locality.py`).

```bash
cd <clone>
L=target/results/probe/login-$(date +%Y%m%d-%H%M); N=target/results/probe/node-$(date +%Y%m%d-%H%M)   # set once, used below
target/probe/probe.sh --only login --out "$L"          # login node, ~5 s on aac7 (m)
salloc -N1 --no-shell -p <partition> -t 30          # or sbatch a hold; note the job id J
target/probe/probe.sh --only node --jobid J --out "$N"  # inside J; srun on the node
#   (if the APUs are hidden from the step: PROBE_SRUN_EXTRA="--gpus-per-node=4" target/probe/probe.sh ...)
target/probe/gen_conf.py "$L" "$N" -o target/config/target.conf          # exactly two positional dirs: the login and the node probe
python3 target/ladder/locality.py nodes --from "$L/raw/scontrol_nodes.txt" --save
```

Check at once (they decide the first runs): `gpu.count` = 4, `net.cxi_count` = 8 and `net.nics_per_apu` = 2,
`shmem.rc.*` = 0 (the Cray SHMEM 12.1 test ran with 1, 2 and 4 PEs and with 2/4/8 NICs), `uffd.*` (the libfabric memory
monitor `FI_MR_CACHE_MONITOR=userfaultfd` needs it: 0/104 hangs against 22/105 with memhooks, m on aac7), `lib.gmp_h`
(ecalc needs GMP; if missing, build it into `$APU/gmp` from source and set `GMP_HOME`), the partition limits in
`slurm.part.*` / `slurm.qos` / `slurm.assoc` (largest job, time limit, jobs per user: they size the worker team, §4).
`gen_conf.py` prints the keys it could not fill; fill them by hand with a `# how determined` comment. Commit the probe
results and `target/config/target.conf` right away (Q112).

## 6. Step 2 — build

Both paths build on the login node in under a minute each (aac7: 24 s and 20 s, m), as bench items `bld01` (path 1)
and `bld02` (path 2) into `target/build/<sha7>-p1|p2/` (copies of `ecalc/` and `tools/`, so both build at once and
`mnrun.sh` sits next to its binary):

```bash
target/bench/run.sh --jobid none --only bld01,bld02      # login-node worker: runs only nodes=0 items
cat target/results/bench/bld0?/summary.txt               # status=PASS, e.binary=build/<sha7>-pN/ecalc/ecalc
```

Modules come from `target/config/target.conf` (`MNRUN_MODULES`, `MNRUN_UNLOAD`; path 2 needs the Cray SHMEM 12.1 and
dsmml modules, `make -B XSHMEM=cray`). GMP: `GMP_HOME` (if the site has none: build GMP 6.x from source into `$APU/gmp`,
about 5 min). If path 2 fails to build, carry on with path 1 (every item and the ladder accept a missing path) and fix
path 2 in parallel. Once both pass, the runner hands `ECALC1` / `ECALC2` to every item, and the ladder finds the newest
binaries itself.

## 7. Step 3 — the bench (parallel workers)

The bench answers the questions in `target/bench/QUESTIONS.md` (63+ rows: band 0-99 ladder-critical, 100-199
optimization, 200+ calibration for offline work; each row names the decision it drives and the model constant it
feeds). Items are listed in `target/bench/manifest.d/*.tsv` (id, tier, prio, nodes, est_min, deps, questions, cmd) and
run once each; arms of an A/B run at the same time on disjoint hosts of the item's allocation instead of repeats.

```bash
target/bench/run.sh --validate                    # manifest check (ids, deps, scripts)
target/bench/run.sh --plan 8                      # how many workers of which sizes drain the list fastest
# one worker = one allocation (the runner gives each item the FIRST hosts of its allocation: never two workers in one job)
mkdir -p target/results/bench/_slurm      # sbatch logs: kept and committed (Q112), never at the clone root
sbatch -N <n> -p <partition> -t <min> --exclusive --gpus-per-node=4 -o target/results/bench/_slurm/%j.out --wrap "cd $PWD && target/bench/run.sh --jobid \$SLURM_JOB_ID --worker w<k>"
target/bench/run.sh --status                      # who runs what, what is done
python3 target/bench/aggregate.py                 # results/bench/METRICS.tsv + ANSWERS.md (+ coverage warnings, progress line)
```

Worker sizes: a few 1-node and 2-node workers take the many small items; one 8-node and one 64-node worker take the
scaling items (`net04`, `net05`, `scl02`, `scl04`, the `cal0*` multi-node points); start the large ones as soon as their
dependencies pass (the runner waits up to `--wait-min M` minutes, default 30, for dependencies, then exits saying what is left: restart it later).
Stale claim (a worker died): `run.sh --reclaim <id>`; redo a FAIL after a fix: `--reclaim <id> --force` (old results move
to `results/bench/_old/`). A failed path-2 build does not block anything: path-2 arms SKIP with a note. After every few
items: `aggregate.py`, commit `target/results/` (coordinator). Read `ANSWERS.md` as it fills: answers in band 0-99 can
change the ladder's settings (ADJUST.md) before L1.

## 8. Step 4 — the ladder (1, 8, 64, 576 nodes)

Sizes are fixed (Q114). The digits are the first campaign's share of **8 × 10⁹ digits per node** (user decision Q120,
2026-10-25: "reduce the number of digits until all the code paths work, and then plan to run those. We can always
increase the number of digits later if the smaller runs succeed"): at 8e9 per node every shape fits at every level —
layout D (path 1), P (path 1, one process per GPU) and path 2 (Cray SHMEM) — while at the 3.82e13 headline's share
(6.632e10 per node) P and path 2 are refused from 8 nodes on (mod, planner 2026-10-25):

| level | nodes (APUs) | digits | replicas in parallel | paths | write | model wall (mod, D, no write / write) | worst plan (path 2, mod) |
|---|---|---|---|---|---|---|---|
| L0 | 1 (4) | 8e9 | 8, 2 runs each | both, order alternating | no | 16.8 / 19.9 s | device 144 GB, node 191 GB |
| L1 | 8 (32) | 6.4e10 | 4, 2 runs | both | no | 22.6 / 24.6 s | device 216 GB, node 293 GB |
| L2 | 64 (256) | 5.12e11 | 2, 2 runs | both | first replica | 25.8 / 27.6 s | device 216 GB, node 293 GB |
| L3 | 576 (2304) | 4.608e12 | 1, 2 runs | both | yes | 32.0 / 33.2 s | device 328.69 GB (+44.75), node 406.32 GB (+7.68 vs 414) |

When all four levels pass on both paths, raise the share step by step (`--set DIGITS=...`; the plan check refuses what
does not fit and prints the largest size that does): the planner's limits per shape at 576 nodes are about 1.2e10 per
node for path 2 (device +18.98 GB, node 440 GB: needs a node budget ≥ 440), and 6.632e10 per node (the 3.82e13
headline) for layout D only with `MN_NONPOW2_CAP=0` (Q119; device 358.96 of 373.44 GB). Short runs make start-up a
large share of the wall: that is what the campaign measures first (init, launch, fabric), so record every phase.
Run 1 of a fresh allocation is a cold start (ADJUST rule 11): every level does 2 runs and is timed from run 2.
`ladder.sh` drops a path whose plan is refused and runs the level on the other one.

```bash
python3 target/ladder/locality.py nodes --save                  # physical table -> results/probe/topology.tsv (once)
target/ladder/ladder.sh --level L0 --dry-run                    # plan check, the groups it would use, the job lines
target/ladder/ladder.sh --level L0                              # submits 8 replicas (one sbatch each, adjacent groups)
target/ladder/ladder.sh --collect L0                            # after the DONE markers: mean/min/max, spread, VERIFY, hash agreement
#   then L1, L2, L3 the same way; inside an allocation you hold: --jobid J; overrides: --set KEY=VALUE (e.g. --set ECALC_NP=4)
```

Each replica is a fire-and-forget driver writing `results/ladder/<L>/<tag>-r<k>/{summary.txt,raw/,DONE}`; nothing waits.
Between runs follow `target/ladder/ADJUST.md`: the gate to move up (every replica VERIFY OK, hashes agree, spread and
time against the model), and the rules (slow node → exclude it in `results/ladder/exclude.txt`; path 1 vs path 2 → keep
the faster for the next level; memory near the edge; init share; the `MN_NONPOW2_CAP` cost from bench `prm08`, with
the fallback of about 2.67e13 digits with it on). L0-L2 can run while bench workers use other nodes; L3 needs the whole
machine, so schedule it when the bench prio < 200 items are done or the session is ending.

## 9. Known problems and their fixes

| symptom | cause and fix (evidence) |
|---|---|
| run hangs in memory registration (ROCr ↔ libfabric) | keep `FI_MR_CACHE_MONITOR=userfaultfd` (the `mnrun.sh` default; 0/104 hangs vs 22/105 with memhooks, m aac7); `disabled` gives WRONG exchanges and is refused. If the site's kernel refuses userfaultfd (probe `uffd.*`, kit stage `uffd`), stop and tell the user before any multi-node run |
| rc 8 at start | the memory plan does not fit (`ECALC_DEVICE_GB` / `ECALC_NODE_GB`): lower the digits (the refusal prints the largest that fits) or change the settings the plan names |
| rc 9 during a run | the memory guard (`ECALC_MEM_GUARD_GB=6`) stopped the run before the OOM killer: lower `ECALC_NODE_GB` by the overshoot it prints |
| rc 134 | `abort()` (core file) — with `ECALC_SEGV_TRACE=1` the trace is in the log; keep the log in `results/` |
| `fi_enable(-28)` with several PEs per node | launch Slurm-native (`srun --ntasks`), as `mnrun.sh` does (TGTBENCH2, m target) |
| a task gets only 2 CPUs | `MNRUN_CPUS_PER_TASK=auto` (mnrun.sh: CPUs / tasks per node) (A16, m aac7) |
| link or load errors after `module load rocm` | a versioned ROCm conflicts with the login default: `MNRUN_UNLOAD` in target.conf (the probe fills it) |
| APUs invisible inside a job step | `--gpus-per-node=4 --gpu-bind=none` (the ladder's default sbatch options; probe: `PROBE_SRUN_EXTRA`) |
| one node slow (a replica 5-10 % behind) or a NIC missing | `tools/nodescreen.sh` / kit stage `nodechk`; add the host to `results/ladder/exclude.txt` (`MNRUN_EXCLUDE_HOSTS`, mode drop); no spares |
| NICs lost after jobs end (aac7: needs_reboot after our jobs) | use long allocations and run many items inside them; check `fi_info -p cxi` counts at the start of each allocation |
| the first run on a fresh allocation is slow | cold start: on aac7 the first run took 154.9 s, later runs on the same node 76.9-85.5 s (m, 2026-10-25); time from run 2 on (ADJUST rule 11). `ECALC_VERBOSE=2` costs at most a few seconds (+2.0 s mean over 4 pairs, not resolved, m) |
| run-to-run spread | aac7 is bimodal (one run in three 5-8 % slower); measure arms at the same time on different nodes instead of repeating |
| start-up hang (E1: 1 sighting in 31 at 7.7e10, 0 in 301 since) | kill only that job by its id, rerun once, record it in `results/LOG.md` |
| an item or driver outlives its allocation | `scancel` only your own job ids, `kill` only single PIDs of your own processes; never touch other users' jobs |
| Cray SHMEM 12.1 (path 2) start-up slow or failing | path 1 is complete on its own; the ladder and every item run with one path; record the failure (probe `shmem.*`, bench `net01`) |

## 10. Before access ends

- Commit and push `target/results/` (the coordinator, every 30-60 min and now).
- `bench/aggregate.py`; check `results/bench/ANSWERS.md` and `METRICS.tsv` are committed.
- Write `target/results/HANDOFF.md`: what ran, what is left (by priority), surprises, the next questions.
