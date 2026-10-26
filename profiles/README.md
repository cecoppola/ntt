# profiles/ -- machine profiles (draft, step one)

Scope: the machine profiles (`*.conf`, the template), their format, what belongs in a profile and how to check one. |
Maintained: updated in the same merge as any change to a profile, the loader/checker or the settings they hold.

Status (O3, merged 2026-10-12): step 2 done. `tools/profile_load.py` validates and loads a profile (explicit environment > profile > code
default); `tools/profile_load.py NAME --run -- cmd` runs a command under it and prints a `config-src:` line; `tools/gate.sh --profile NAME` uses it, and
`gate.sh --build` runs `switch_registry.py --check` and `profile_load.py --validate-all` first. Nothing reads a profile unless asked. Details: results/TOOLS_PROFILES.md §O3.
T27 (2026-10-12): the computed values have one source. `ecalc/mnrun.sh` computes them for every launch (the pool from the planner,
`MN_PLAN_ONLY`; the heap; `FI_UNIVERSE_SIZE`), the code computes `MN_GROUPS` (its default schedule); `tools/plan_values.py
[--profile P] [--line FILE] [--exports] DIGITS NODES` prints them without launching (`MNRUN_PRINT_ENV=1`), compares a hand line
with them (MATCH / DIFF / NO-CONSUMER) and gives `export` lines for a launch that does not go through `mnrun.sh`. Details: results/TOOLS_PROFILES.md §T27.

FIT (D26, 2026-10-13, `results/LAUNCH_DEFAULTS.md §FIT`): `ECALC_DEVICE_GB` -- the device edge per node, measured per machine -- is a profile key
(aac7 444.0, target 373.44 = the code default); the planner's memory fit (`tools/plan_values.py --fit`) puts the run's device peak
against it, and `mnrun.sh` / `tools/gate.sh` refuse a run that does not fit. aac7 carries it on its launch line (`LAUNCH_LINE_KEYS`).

## Plan (agreed 2026-10-11)

Adopted settings become built-in defaults in the code. What stays machine-specific lives in a named profile here. One entry
point (later step) loads, in this order, **built-in defaults < profile < overrides** (the caller's environment / command line),
validates, and prints the effective configuration and its hash. Size-dependent values are never typed: the plan stage computes them.

## Files

| file | machine | status |
|---|---|---|
| `aac7.conf` | aac7 (Cray EX, 4 x MI300A, Slingshot-11, Cray OpenSHMEMX) | filled from `ecalc/aac7env.sh`, `mnrun.sh`, the launch lines in `PLAN.md` §3; holds the aac7 launch line (`LAUNCH_LINE_KEYS`) |
| (moved) `target/config/target.conf.template` | the production machine | every unknown is `__FILL__` with "how to determine"; `target/probe/gen_conf.py` fills it into `target/config/target.conf` (generated on the target, committed) |

Site specifics of the target (names, module names, paths, node lists) must never be committed here. The tracked template holds
generic values only; the filled copy stays out of git.

## Format

* One `KEY=value` per line, shell-sourceable (`. profile.conf` must work). No `export`, no command substitution, no logic.
* Value with spaces: double quotes (`MNRUN_MODULES="a b c"`). `$HOME` / `${HOME}` may appear in path values; the loader expands
  them.
* `#` starts a comment, also after a value. Section headers are comment lines of the form `# ---- [name] ----`; the section names are
  `modules/build`, `scheduler`, `paths`, `transport`, `libfabric`, `memory pools tied to the hardware`, `exchange tuning`,
  `adopted ... pending move to built-in defaults`, `safety/diagnostics`, `launch line`, `host exclusions`.
* `__FILL__` is the value of an unknown. The validator must refuse a profile that still has one (the checker lists them as UNFILLED).
* A key to be **unset** is not expressible yet (the target's record run unsets `ECALC_CHECKPOINT ECALC_CKPT_TOP BS_CKPT_DIR`): the
  entry point needs an `unset` section; open question in `results/TOOLS_PROFILES.md §C01`.
* Each key carries its evidence in the trailing comment: the `ecalc/README.md` switch row, `results/<ID>.md`, or the user's decision with its
  date. Numbers are labelled (m) measured, (mod) modelled, (a) assumed. A line without evidence is a bug in the profile.

## What belongs where

| goes to | rule | examples |
|---|---|---|
| **profile** | a fact about the machine or site that no measurement on another machine would change: modules, SHMEM implementation, transport choice, NIC/fabric settings, memory pools bounded by the hardware, scheduler partition, scratch and output paths, excluded hosts, per-site safety limits | `MNRUN_MODULES`, `ECALC_DEVICE_GB`, `FI_MR_CACHE_MONITOR`, `MNRUN_EXCLUDE_HOSTS`, `MNACCEPT_TMP` |
| **built-in default** (code) | a choice the user adopted that is right on every supported machine, or a safe value of a tunable | `ECALC_NP=auto`, `MN_T_CHUNK_MB`; `MN_OUT_DKM_HI=1` and the exchange v-slot sharing are built in without a switch (2026-10-12, TASKS 27) |
| **computed** (plan stage: `tools/plan_values.py`) | anything that is a function of the digit count, node count and hardware sizes | `SHMEM_SYMMETRIC_SIZE`, `XT_SYMMETRIC_HEAP_SIZE` (pool + 512 MiB), `COMM_SHMEM_POOL_MB`, `XSHMEM_HEAP_MB`, `FI_UNIVERSE_SIZE` (max(4096, 4 x tasks)), `MN_GROUPS` at a given size |
| **override** | one run's deliberate deviation (an A/B arm, a test) | `--env` of `tools/gate.sh`, the caller's environment |

Transitional section: today some adopted switches (T2048, `ECALC_NP`, ...) are on the launch lines but not yet code defaults.
They are listed in the profile under `adopted ... pending move to built-in defaults` so that nothing is lost; as each becomes a code
default its line is deleted from every profile (the checker then shows it as SAME-AS-SCRIPT where a script repeats it, the cue to delete it).
A profile holds only values that differ from the code's and the scripts' own defaults (Q107): `aac7env.sh` and `mnrun.sh` keep the module, partition, scratch and libfabric defaults of aac7; the checker shows a key equal to a script default as SAME-AS-SCRIPT, the cue to delete it.

## The launch line (SCR2, D15, 2026-10-12)

`LAUNCH_LINE_KEYS="K1 K2 ..."` (a meta key: described here, never exported) names the profile keys that go on the command line of every
timed or gated launch, in order. `tools/profile_load.py NAME --line` prints them as `KEY=value` words; `tools/gate.sh --line aac7` uses
exactly that, so the line has one source (the values above it in the same file). The old holder, the script `e16_headline.sh`, is archived.

## Checking a profile

```
tools/profile_load.py aac7 --check                 # drift check: keys that repeat a script default, unknown or unfilled keys
tools/profile_load.py aac7 --line                  # the launch line (the keys of LAUNCH_LINE_KEYS as KEY=value); tools/gate.sh --line aac7 runs it
tools/profile_load.py aac7 --check -q              # problems only; --strict exits 1 on SAME-AS-SCRIPT / UNKNOWN
```

Statuses: SAME-AS-SCRIPT (a literal `${VAR:-default}` in `mnrun.sh`, `aac7env.sh` or `target/kit/target_kit.sh` equals the profile value: delete the key), OVERRIDE (the
script defaults to another value; the profile wins), PROFILE-ONLY (no script defaults it), UNFILLED, UNKNOWN (no such name anywhere in `ecalc/` or
`tools/`: a typo or a key without a consumer). Read-only.

## Target bring-up

Superseded: `target/RESTART.md` §5 (facts and `target.conf` via `target/probe/probe.sh` + `target/probe/gen_conf.py`; every unknown key and how to
determine it is in `target/config/target.conf.template`), §6 (build), §7 (bench), §9 (known problems). No `profiles/target.conf` exists; the
target config is `target/config/target.conf` (read by `target/lib/common.sh` and `target/ladder/ladder.sh`).

## First SHMEM run on the target

Background and evidence: `results/SHMEM.md` (cause of the aac7 stalls, the stall checklist and the target implications, condensed) and `archive:results_merged_2026-10-23/SHMEM_STALLS.md` (the full report: causes sections 4-5; target implications section 9; hugetlb switch 9.1; healthy values 8.3; stall checklist 8.4). The SHMEM transport depends on three things outside ecalc: kernel memory settings, the GPU driver's queue eviction
when the kernel moves pages, and the NIC count per node. The probe (node phase, `--hold`) reads them; none is changed by it.

| probe fact | meaning | switch choice |
|---|---|---|
| `KERNEL`, `KERNEL_GE_6_1`, `MADV_COLLAPSE` | `works` needs kernel >= 6.1 or a backport (the probe tests it on an anonymous mapping) | `works`: the node-process heap collapses the heaps to 2 MiB pages, the fix of the aac7 stalls. `errno=22` on an idle node (with `KERNEL_GE_6_1=no`, or the same on a repeat): unsupported; on a busy host it can also mean no free 2 MiB block. The run prints a collapse failure line and keeps 4 KiB heap pages, which are the pages compaction migrates (the stall cause): then use the hugetlb heap if a pool exists, else expect stalls and ask the site for a backport or `compaction_proactiveness=0` |
| `HUGETLB_2M_FREE`, `_NR`, `_PER_NUMA`, `_OVERCOMMIT` | the 2 MiB pool; per NUMA node `free/total` | pool present: reserve about heap size / 2 MiB per node-process (plus the library's own needs) and A/B the libsma heap on hugetlb (path 2, `target/bench/QUESTIONS.md` TQ37). No pool (aac7: 0, overcommit 0): the switch prints needed and free pages and falls back to `NP_HEAP_THP`; reserve at boot or in a job prolog, before memory fragments |
| `THP_enabled`, `THP_defrag`, `THP_shmem_enabled`, `THP_khugepaged_defrag` | aac7: `always`/`madvise`/`never`; AMD's MI300A guidance wants THP `always` for unified memory | `enabled=never` makes MADV_COLLAPSE fail: stop and ask the site. `shmem_enabled` other than `never` or a `huge=` on `/dev/shm` changes how the SHMEM heap file is backed: record it, re-run the 2-node gate |
| `KERNEL_MM_COMPACTION_PROACTIVENESS`, `_COMPACT_UNEVICTABLE_ALLOWED` | aac7 and AMD's guide: 20 and 1 | keep both for the first run. `0` is an A/B per job (stalls, KFD `evicted_ms`, GPU allocation time at start-up), not a default; only if stalls show up in the checklist below |
| `BUDDY_FRAG_SCORE`, `BUDDY_FREE_GE2M_MB` | per NUMA node and zone: share of free memory in blocks below 2 MiB (above 90 starts a proactive kcompactd pass); free MiB in blocks of 2 MiB or more | an idle node should be low (aac7 clean runs 1-17; start-up itself pushes node 0 of the first node to 91-99). A high score before the job: compact or drop caches in a prolog |
| `CXI_DEV_COUNT`, `FI_CXI_DOMAINS` | NICs per node (8 on the target, 4 on aac7) | `SHMEM_OFI_NUM_NICS` defaults to 1: set it to the NIC count with a NUMA or user mapping (the launcher caps it; the library aborts when asked for more NICs than exist). `target/RESTART.md` §5 |
| `KFD_PRESENT` | the GPU driver's per-process statistics exist | needed for the `evicted_ms` column below |

Checklist for the first SHMEM run (after steps 0-9; 2 nodes, `1e10` digits or larger, then 4 nodes), adding to the launch line:

1. Read the log: the node-process heap line (`heap ... MiB PMD-mapped after`) shows whether the heap was collapsed to 2 MiB pages; a shortfall means 4 KiB pages are left: see the `MADV_COLLAPSE` row. (The communication trace and its report were removed with the plain SHMEM transport, Q103; the stall checklist of `results/SHMEM.md` uses `/proc/vmstat` deltas and the KFD counters.)
2. Stalls present: follow the checklist in `results/SHMEM.md` (per-second `/proc/vmstat` deltas on the late node, kcompactd ticks, score above 90, KFD `evicted_ms`), then try in this order: hugetlb heap if a pool exists; `compaction_proactiveness=0` per job (A/B with an A/A control, at least 4 ABBA pairs); a prolog that compacts and drops caches.
3. Measure the stall rate per node-hour on a short run before assuming it is zero: at 576 nodes one frozen node costs everyone (section 6.4 of the archived report).
4. Compare SHMEM (path 2) against libfabric (path 1), same digits and nodes, A/A first; time from run 2 on (the first run of an allocation is a cold start, `target/RESTART.md` §9).

### SHMEM settings to use on the target

Start with none of the stall switches on the launch line; the node-process heap collapse to 2 MiB pages is on by default (the stall fix).
- Measure first, in this order: (1) the collapse lines and the stall checklist above (hugetlb pool present: the libsma heap on hugetlb against off, 4 nodes, path 2); (2) SHMEM against libfabric at 2, 4 and 8 nodes with the same digits per node, to see whether the aac7 gap (parity at 1 peer, about +7 % at 3, about +9 % at 5) grows with the peers on the target fabric; (3) a repeat of the 2-node pair as a control.
