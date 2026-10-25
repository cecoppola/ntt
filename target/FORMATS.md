# target/FORMATS.md — shared conventions of the target folder

Scope: the layout of `target/`, the contracts between its scripts (probe, bench runner, items, ladder, locality), the
result formats, and the rules every agent working in `target/` follows. Read this before writing or running anything
here. Entry point for a new session: `target/RESTART.md`.

## 1. Folder layout

| path | what | written by |
|---|---|---|
| `RESTART.md` | full context for the agent on the target: mission, steps 1-4, rules, install steps | here (before access) |
| `README.md` | one-page map and the run order | here |
| `FORMATS.md` | this file | here |
| `governance/` | copies of the rule files (CLAUDE.md files, coordinator rules, settings, hooks, agents, memory); install steps in RESTART.md | here |
| `lib/common.sh` | shared shell functions (§3); every script sources it | here |
| `probe/` | step 2: `probe.sh` (fast system facts), `probe_shmem.c`, `gen_conf.py` (facts → `config/target.conf`) | here |
| `config/` | `target.conf.template`; `target.conf` (generated on the target, committed) | template here, conf on target |
| `bench/` | step 3: `manifest.tsv`, `QUESTIONS.md` (question register), `run.sh` (claiming runner), `items/<id>.sh`, `aggregate.py` | here |
| `ladder/` | step 4: `ladder.sh`, `locality.py`, `levels/L0..L3.conf`, `ADJUST.md` (rules for changes between runs) | here |
| `kit/` | `target_kit.sh` (the older staged kit; bench items call its stages) | moved here |
| `build/` | binaries built on the target (`build/<sha7>-<variant>/ecalc`); never committed | target |
| `results/` | everything measured on the target, committed raw (decision Q112) | target |

`results/` layout:

```
results/probe/<host>-<YYYYmmdd-HHMM>/   probe output (facts.tsv, raw/*.txt, summary.txt)
results/probe/topology.tsv              host -> physical location table (locality.py nodes)
results/bench/<id>/                     one dir per bench item: claim, summary.txt, raw/, DONE
results/bench/METRICS.tsv               all m.* values of all items (aggregate.py)
results/bench/ANSWERS.md                question register with the answers filled (aggregate.py)
results/ladder/<level>/<run>/           one dir per ladder run: summary.txt, raw/, DONE
results/LOG.md                          the coordinator's dated log of the session (times with time zone)
```

## 2. Roles on the target

- **Coordinator** (the main session): the only process that runs `git` (pull, commit, push). It commits `results/`
  every 30-60 min and at each milestone, so a session cut short loses at most one interval.
- **Workers** (agents or plain shells): each holds its own Slurm allocation and loops `bench/run.sh` or one ladder
  command. A worker writes only below its own `results/.../<id>/` dir, `build/` (build items only) and `/tmp`.
- Claims are atomic `mkdir` locks, so any number of workers can share one clone on the shared file system.
- No item may assume another item runs at the same time; dependencies are declared in the manifest only.

## 3. lib/common.sh (source it: `. "$(dirname "$0")/../lib/common.sh"` or via `$TGT_ROOT`)

| function | does |
|---|---|
| `tgt_init` | sets `TGT_ROOT` (the `target/` dir), `REPO` (the repository root), `TGT_RES` (`$TGT_ROOT/results`), `TGT_SHA` (7-char HEAD) |
| `tgt_conf` | loads `config/target.conf` if present (KEY=VALUE lines, `#` comments); refuses keys still `__FILL__` only when `TGT_STRICT=1` |
| `tgt_log MSG` | timestamped line to stderr and to `$OUT/log.txt` when `OUT` is set |
| `tgt_sum_init DIR ID` | creates `DIR/raw/`, starts `DIR/summary.txt` (id, start, host, commit) |
| `tgt_kv KEY VALUE` | appends `KEY=VALUE` to the current summary (one line; newlines replaced by ` / `) |
| `tgt_sum_end STATUS RC` | appends end, wall_s, status, rc; writes `DONE` (contains the rc) |
| `tgt_claim DIR` | atomic claim (`mkdir DIR/claim.d`); returns 1 if someone else holds it |
| `tgt_timeout MIN CMD...` | runs CMD under `timeout` with MIN minutes, kills the process group on expiry, returns 124 |
| `tgt_hosts JOBID` | prints the allocation's hosts one per line (`scontrol show hostnames`) |

## 4. Bench manifest — `bench/manifest.tsv`

`bench/manifest.tsv` plus every `bench/manifest.d/*.tsv` (one fragment per author or area, so parallel authors never edit
the same file; the runner reads them all, ids must be unique across them). Tab-separated, one item per line, header line
first, `#` lines are comments:

```
id	tier	prio	nodes	est_min	deps	questions	cmd
```

| column | meaning |
|---|---|
| `id` | `<area><nn>` lowercase, unique, also the results dir name (areas: `sys`, `bld`, `mem`, `gpu`, `ntt`, `net`, `io`, `ini`, `scl`, `prm`, `cal`, `cor`) |
| `tier` | 1 = short (≤ 10 min wall), 2 = medium (≤ 45 min) |
| `prio` | integer, lower runs first; 0-99 = needed by the ladder's first run, 100-199 = answers an open optimization question, 200+ = calibration for later offline work |
| `nodes` | nodes the item needs (1, 2, 4, 8, 16, 64 …); the runner gives an item exactly this many hosts, adjacent ones first |
| `est_min` | expected wall minutes including setup; the runner kills the item at 2 × `est_min` + 2 |
| `deps` | comma list of ids that must be DONE with status PASS or PARTIAL, or `-` |
| `questions` | comma list of register ids (`TQ<nn>`) the item answers, or `-` |
| `cmd` | path below `bench/items/` plus fixed arguments, e.g. `net01.sh --sizes 4K,64K,4M` |

Items are short and run once (no repeats unless the item itself needs n > 1 for a spread, then n is fixed in the item).

## 5. Item contract — `bench/items/<id>.sh`

Called by `bench/run.sh` with the environment below; the current dir is `$OUT`.

| variable | meaning |
|---|---|
| `TGT_ROOT`, `REPO`, `TGT_SHA` | as in §3 |
| `OUT` | the item's results dir (exists, has `raw/`) |
| `ID` | the item id |
| `JOBID` | the Slurm allocation to `srun --jobid` into |
| `NODES` | number of hosts given to the item |
| `HOSTS` | comma list of those hosts, in physical order (locality.py) |
| `ECALC1` | path-1 binary (default build, libfabric transport) — set once `bld01` is DONE |
| `ECALC2` | path-2 binary (`XSHMEM=cray`, run with `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`) — set once `bld02` is DONE |
| `TCONF` | `config/target.conf` (already loaded into the environment) |

Rules: write raw output to `$OUT/raw/`; put results in `summary.txt` with `tgt_kv` (§6); exit 0 when the measurement
was taken (also when the answer is unfavourable), nonzero when it failed. Never `scancel`, never touch other jobs; use
`srun --jobid "$JOBID" -N <n> -w <hosts>`. No literal host names, IPs or user names in scripts (they are found at run time).

## 6. summary.txt (all results: probe, bench, ladder)

`KEY=VALUE` lines, one fact per line, order free; written through `lib/common.sh`:

```
id=net01                       item / run id
status=PASS|PARTIAL|FAIL|SKIP  PARTIAL: some sub-measurements missing; SKIP: precondition absent (say why in note)
rc=0
start=2026-11-03T14:05:11-05:00   ISO 8601 with offset
end=...    wall_s=412
host=<first host>   nodes=2   hosts=<comma list>   jobid=<id>   commit=<sha7>
q.TQ07=<one-line answer with the number and its unit>
m.<metric>=<value> <unit>      measured numbers (e.g. m.put_bw_4M=23.4 GB/s); names lowercase [a-z0-9_.]
e.<NAME>=<value>               environment / settings used (e.g. e.MN_T_CHUNK_MB=2048)
note=<free text>
```

`aggregate.py` turns every `m.*` into a row of `results/bench/METRICS.tsv` (`id metric value unit`) and every `q.*`
into the Answer column of `results/bench/ANSWERS.md`.

## 7. Question register — `bench/QUESTIONS.md`

One table, one row per open question about the target: `TQ<nn> | question | why it matters (the decision it drives) |
items | prio`. Every manifest item names at least one `TQ`; every `TQ` has at least one item. Add questions, never
renumber.

## 8. config/target.conf

The keys of `config/target.conf.template` (KEY=VALUE, `#` comments, evidence tags (m)/(mod)/(a)/(u)). `probe/gen_conf.py
results/probe/<dir>` fills every key the probe determines and leaves the rest at `__FILL__` with a reason comment.
Scripts load it through `tgt_conf`; `ecalc/mnrun.sh` reads the same keys from the environment.

## 9. Locality interface — `ladder/locality.py`

```
locality.py nodes [--from sinfo|FILE]           table: host xname cab chassis slot blade node state (writes results/probe/topology.tsv with --save)
locality.py pick --size N [--count K] [--state idle|alloc|any] [--exclude FILE]
                                                 K disjoint groups of N physically adjacent hosts, one group per line,
                                                 comma separated, in physical order; exit 3 if fewer than K groups exist
locality.py order HOSTLIST                       the same hosts sorted by physical location (rank order for the run)
```

Physical order is (cabinet, chassis, slot, blade, node) parsed from the HPE xname (`x<cab>c<chassis>s<slot>b<blade>n<node>`,
from `scontrol show node` NodeAddr/NodeHostName or the site's mapping). A group of N lies inside the smallest unit that
holds N nodes (blade/slot → chassis → cabinet → system); `pick` reports `cross=<unit>` on stderr when that is impossible.
Ladder sizes are fixed (decision Q114): L0 1 node (4 APUs), L1 8 nodes (32), L2 64 nodes (256), L3 576 nodes (2304).

## 10. Ladder

`ladder/ladder.sh --level L0|L1|L2|L3 [--replicas R] [--run-tag T] [--set KEY=VALUE ...]` reads `ladder/levels/<level>.conf`
(NODES, DIGITS, PATH=1|2, RUNS, env settings), picks R disjoint adjacent groups (`locality.py pick`), checks the memory
plan first (`MN_PLAN_ONLY`), runs ecalc on each group (replicas in parallel, one allocation each), verifies the digits,
and writes `results/ladder/<level>/<run>/summary.txt`. `ladder/ADJUST.md` says which measured numbers change which
settings before the next run and when to move up a level.

## 11. Rules for every agent in target/

- Correctness first: a ladder run counts only with its digit check PASS.
- Label every number m (measured) / mod (modelled) / a (assumed) in prose; times with the time zone.
- Raw target data is committed as it is (Q112). Never commit passwords, keys or tokens (Q116).
- Run each bench item once; fast over exhaustive. Priority order everywhere (Q115): the session may end at any time.
- Agents never ask the user directly; the coordinator collects decisions with options.
