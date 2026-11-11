# profiles/ -- machine profiles (draft, step one)

Scope: the machine profiles (`*.conf`, the template), their format, what belongs in a profile and how to check one. |
Maintained: updated in the same merge as any change to a profile, the loader/checker or the settings they hold.

Status: `tools/profile_load.py` validates and loads a profile (explicit environment > profile > code
default); `tools/profile_load.py NAME --run -- cmd` runs a command under it and prints a `config-src:` line; `tools/gate.sh --profile NAME` uses it, and
`gate.sh --build` runs `switch_registry.py --check` and `profile_load.py --validate-all` first. Nothing reads a profile unless asked. Details: results/CODE_TOOLS.md §O3.
T27 (2026-10-12): the computed values have one source. `ecalc/mnrun.sh` computes them for every launch (the pool from the planner,
`MN_PLAN_ONLY`; the heap; `FI_UNIVERSE_SIZE`), the code computes `MN_GROUPS` (its default schedule); `tools/plan_values.py
[--profile P] [--line FILE] [--exports] DIGITS NODES` prints them without launching (`MNRUN_PRINT_ENV=1`), compares a hand line
with them (MATCH / DIFF / NO-CONSUMER) and gives `export` lines for a launch that does not go through `mnrun.sh`. Details: results/CODE_TOOLS.md §T27.

FIT (D26, 2026-10-13, `results/MODEL.md §FIT`): `ECALC_DEVICE_GB` -- the device edge per node, measured per machine -- is a profile key
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
  `memory budget`, `exchange tuning`, `safety/diagnostics`, `launch line`, `host exclusions` (`aac7.conf`); the target template adds
  `modules/build`, `scheduler`, `paths`, `transport`, `libfabric`, `memory pools tied to the hardware`.
* `__FILL__` is the value of an unknown. The validator must refuse a profile that still has one (the checker lists them as UNFILLED).
* A profile cannot unset a key (an `unset` section does not exist): leave the key out and unset it on the launch line.
* Each key carries its evidence in the trailing comment: the `ecalc/README.md` switch row, `results/<ID>.md`, or the user's decision with its
  date. Numbers are labelled (m) measured, (mod) modelled, (a) assumed. A line without evidence is a bug in the profile.

## What belongs where

| goes to | rule | examples |
|---|---|---|
| **profile** | a fact about the machine or site that no measurement on another machine would change: modules, SHMEM implementation, transport choice, NIC/fabric settings, memory pools bounded by the hardware, scheduler partition, scratch and output paths, excluded hosts, per-site safety limits | `MNRUN_MODULES`, `ECALC_DEVICE_GB`, `FI_MR_CACHE_MONITOR`, `MNRUN_EXCLUDE_HOSTS`, `MNACCEPT_TMP` |
| **built-in default** (code) | a choice the user adopted that is right on every supported machine, or a safe value of a tunable | `ECALC_NP=auto`, `MN_T_CHUNK_MB`; `MN_OUT_DKM_HI=1` and the exchange v-slot sharing are built in without a switch (2026-10-12, TASKS 27) |
| **computed** (plan stage: `tools/plan_values.py`) | anything that is a function of the digit count, node count and hardware sizes | `SHMEM_SYMMETRIC_SIZE`, `XT_SYMMETRIC_HEAP_SIZE` (pool + 512 MiB), `COMM_SHMEM_POOL_MB`, `XSHMEM_HEAP_MB`, `FI_UNIVERSE_SIZE` (max(4096, 4 x tasks)), `MN_GROUPS` at a given size |
| **override** | one run's deliberate deviation (an A/B arm, a test) | `--env` of `tools/gate.sh`, the caller's environment |

Adopted switches that are not yet code defaults (`MN_T_CHUNK_MB=2048` in `aac7.conf`) sit in their own section; when one becomes a code
default its line is deleted from every profile. A profile holds only values that differ from the code's and the scripts' own defaults (Q107): `aac7env.sh` and `mnrun.sh` keep the module, partition, scratch and libfabric defaults of aac7 (a key equal to a script default shows as SAME-AS-SCRIPT: delete it).

## The launch line (SCR2, D15, 2026-10-12)

`LAUNCH_LINE_KEYS="K1 K2 ..."` (a meta key: described here, never exported) names the profile keys that go on the command line of every
timed or gated launch, in order. `tools/profile_load.py NAME --line` prints them as `KEY=value` words; `tools/gate.sh --line aac7` uses
exactly that, so the line has one source (the values above it in the same file).

## Checking a profile

```
tools/profile_load.py aac7 --check                 # drift check: keys that repeat a script default, unknown or unfilled keys
tools/profile_load.py aac7 --line                  # the launch line (the keys of LAUNCH_LINE_KEYS as KEY=value); tools/gate.sh --line aac7 runs it
tools/profile_load.py aac7 --check -q              # problems only; --strict exits 1 on SAME-AS-SCRIPT / UNKNOWN
```

Statuses: SAME-AS-SCRIPT (a literal `${VAR:-default}` in `mnrun.sh` or `aac7env.sh` equals the profile value: delete the key), OVERRIDE (the
script defaults to another value; the profile wins), PROFILE-ONLY (no script defaults it), UNFILLED, UNKNOWN (no such name anywhere in `ecalc/` or
`tools/`: a typo or a key without a consumer). Read-only.

## Target

No `profiles/target.conf` exists. The target config is `target/config/target.conf`, generated on the target from `target/config/target.conf.template`
by `target/probe/gen_conf.py` and read by `target/lib/common.sh` and `target/ladder/ladder.sh` (procedure: `target/RESTART.md` §2-§3).

## Path 2 (Cray SHMEM) and the kernel memory settings

Evidence: `results/TRANSPORT.md` (sections craylane and SHMEM: the multi-second stalls on aac7 were kernel compaction of a pinned 4 KiB symmetric heap, which evicts GPU queues). Only
`XSHMEM_CRAY_HEAP_THP` (default 0) touches the libsma heap now: `2` reports its backing per PE (smaps line), `1` also asks for 2 MiB pages (`MADV_COLLAPSE`). The probe
(`target/probe/probe.sh`, node part) records the settings; it changes none.

| probe fact (`facts.tsv`) | use |
|---|---|
| `uffd.kernel_ge_6_1`, `uffd.madv_collapse` | `works` = kernel >= 6.1 or a backport; `XSHMEM_CRAY_HEAP_THP=1` needs it |
| `mem.hugetlb_2m_nr`, `_free`, `_per_numa` | a 2 MiB pool lets the libsma heap sit on hugetlb (A/B it on path 2); none on aac7 |
| `mem.thp_enabled`, `mem.thp_defrag`, `mem.thp_shmem` | `enabled=never` makes `MADV_COLLAPSE` fail: stop and ask the site |
| `mem.compaction_proactiveness` | aac7 and AMD's guide: 20; `0` is a per-job A/B (with an A/A control, at least 4 ABBA pairs), not a default |
| `net.cxi_count`, `net.nics_per_apu` | NICs per node (8 on the target, 4 on aac7); `SHMEM_OFI_NUM_NICS` defaults to 1: set it to the NIC count (the launcher caps it) |
| `gpu.kfd` | the GPU driver's per-process statistics exist (`evicted_ms` for the stall checklist) |

First path 2 run on a new machine (2 nodes, 1e10 digits or more, then 4 nodes): no stall switch on the launch line; read the heap line of `XSHMEM_CRAY_HEAP_THP=2`; if
stalls appear, follow the `results/TRANSPORT.md` stall notes (per-second `/proc/vmstat` deltas, kcompactd, KFD `evicted_ms`), then try the hugetlb heap, then `compaction_proactiveness=0`.
Compare path 2 with path 1 at the same digits and nodes (A/A first; time from run 2: the first run of an allocation is a cold start).
