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
| `target.conf.template` | the production machine | every unknown is `__FILL__` with "how to determine"; copy to `target.conf` (not tracked: add it to `.git/info/exclude`) |

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
| **built-in default** (code) | a choice the user adopted that is right on every supported machine, or a safe value of a tunable | `ECALC_NP=auto`, `MN_OUT_DKM_HI=1`, `MN_T_CHUNK_MB`, `COMM_LAYER_VSLOT_*` (built in 2026-10-12, TASKS 27) |
| **computed** (plan stage: `tools/plan_values.py`) | anything that is a function of the digit count, node count and hardware sizes | `SHMEM_SYMMETRIC_SIZE`, `XT_SYMMETRIC_HEAP_SIZE` (pool + 512 MiB), `COMM_OFI_POOL_MB`, `FI_UNIVERSE_SIZE` (max(4096, 4 x tasks)), `MN_GROUPS` at a given size |
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

Statuses: SAME-AS-SCRIPT (a literal `${VAR:-default}` in `mnrun.sh`, `aac7env.sh` or `target_kit.sh` equals the profile value: delete the key), OVERRIDE (the
script defaults to another value; the profile wins), PROFILE-ONLY (no script defaults it), UNFILLED, UNKNOWN (no such name anywhere in `ecalc/` or
`tools/`: a typo or a key without a consumer). Read-only.

## Target bring-up runbook (TASKS 28)

`tools/target_probe.sh` collects the facts and prints a proposed `target.conf` plus its diff against the template (read-only; writes only under
`--out`; output scrubbed of user and host names). Copy the filled file to `profiles/target.conf` (not tracked) and run
`tools/profile_load.py target --validate` (no `__FILL__` may remain). Steps in order; stop at the first failure and fix it before going on.

0. **Probe.** On a login node: `tools/target_probe.sh --out DIR` (modules, Slurm, login facts; the SHMEM test is only built). Then inside a
   one-node allocation: `tools/target_probe.sh --out DIR --hold <job>` (node facts, SHMEM runs: 1/2/4/5 PEs, `SHMEM_LOCALITY_ONNODE`,
   `SHMEM_OFI_NUM_NICS` 1/2/4/8, three NIC policies, about 5 min). `--want-rocm/--want-sma/--want-fi` give the stated versions; the probe
   matches module names by them (no names are typed anywhere). If the site hides the APUs from a step, `PROBE_SRUN_EXTRA="--gpus-per-node=4"`.
1. **Modules** (`TARGET_ROCM`, `TARGET_SMA_MODULE`, `TARGET_DSMML_MODULE`, `MNRUN_MODULES`, `MNRUN_UNLOAD`). From `module avail`: the ROCm module whose
   version is the stated one, the OpenSHMEM module (`cray-openshmemx` or `cray-shmem` family) of the stated version, the highest dsmml. Check
   `MODULE_LOAD_RC=rc=0` and `MODULES_AFTER_LOAD` in the facts: a ROCm module loaded by default conflicts and the later `module load` silently
   fails (results/AAC7_PORT.md §ROCM), so `MNRUN_UNLOAD` = every default-loaded ROCm module (`MOD_LOADED_ROCM`); empty only if none is loaded.
   Expected: three names; the SHMEM library's libfabric (`SMA_LIBFABRIC_RESOLVED`) is the stated libfabric version.
2. **Build** (`GMP_HOME`; path 1 is the default build `make -B GMP_HOME=...` with `XSHMEM=ofi`; path 2 of `README.md` is `make -B XSHMEM=cray GMP_HOME=...`). `tools/gate.sh --build` on a login node or in the allocation; the log must show `-DXSHMEM_OFI` (path 1) or `-DXSHMEM_CRAY` (path 2). `GMP_HOME`: a
   `gmp.h` on the include path (probe note "a gmp.h is available") means delete the key; otherwise build GMP from source and set it to the
   prefix (`ls $GMP_HOME/include/gmp.h`). The Makefile's `CRAY_SMA`/`CRAY_DSMML` defaults point at one version: let the module's
   `CRAY_OPENSHMEMX_PREFIX`/`CRAY_DSMML_PREFIX` win (the probe prints both as `SMA_PREFIX`, `DSMML_PREFIX`). Expected: the build passes with the
   12.x library; if a symbol is missing it is a code issue (priority class 1), not a profile issue.
3. **NIC / fabric** (`COMM_OFI`, `COMM_OFI_NICS`). `CXI_DEV_COUNT` and `FI_CXI_DOMAINS` per node should equal the stated NIC count (8); `CXI_NUMA_MAP`
   against `APU_NUMA_MAP` should give 2 NICs on each APU's NUMA node; `CXI_LINK_SPEED_MBPS` the stated line rate (400000). Fewer NICs than stated on
   a node: that node goes to `MNRUN_EXCLUDE_HOSTS` (step 6). If the map is not 2 per APU, set `COMM_OFI_NICS` (e.g. `0,4;1,5;2,6;3,7`); otherwise leave it unset.
4. **libfabric monitor** (`FI_MR_CACHE_MONITOR`). The profile value is `userfaultfd` (memhooks hangs ROCr, S46). `UFFD_SYSCALL` must show
   `user_mode_only:works` (or plain works): then `FI_MONITOR_userfaultfd=accepted`. If both fail, `vm.unprivileged_userfaultfd` is 0 and the process lacks the capability:
   stop and ask the site; `disabled` is refused by the provider and is not a fallback. Then `ecalc/target_kit.sh` stage `uffd` for the full test.
5. **Memory edge / FIT** (`ECALC_DEVICE_GB`, `ECALC_NODE_GB`). `APU_COUNT` = 4; `APU_VRAM_TOTAL_B` and `APU_GTT_TOTAL_B` are the unified-memory sizes (one
   128 GB pool per APU on an MI300A, shared with the host). `ECALC_DEVICE_GB` is **measured**, not read: the kit's edge stage (`t_edge`, D26) on one node; expected
   about 373 (4 x 93.4 GB; the template value, m on the earlier target data). `ECALC_NODE_GB` = `MemAvailable` on an idle compute node (the probe subtracts 4 GB for the OS and
   only proposes it with `--hold`; a login node's figure is wrong), expected 450-500 on a 512 GB node (aac7: 418-420 measured, the profile sets 420); the planner's fit (`tools/plan_values.py --fit`) must pass at the headline size.
6. **Node screen.** `tools/nodescreen.sh --hold <job> --want-nics 8` over the nodes of a small allocation, then (at scale, before a record run) over all of them; exit 3 = flagged nodes,
   `EXCLUDE` lists them: copy to `MNRUN_EXCLUDE_HOSTS` (comma list; `MNRUN_EXCLUDE_MODE=drop`; the job must ask for the nodes it will use). Expected: none or a few
   per hundred nodes; a node with fewer NICs than `SHMEM_OFI_NUM_NICS` aborts a SHMEM launch (results/NODE_SLOWMODE.md §NIC).
7. **Plan check.** `tools/plan_values.py --profile target 38200000000000 576` (digits and nodes of the headline run) prints the pool, heap, `FI_UNIVERSE_SIZE`, `MN_GROUPS` and the memory fit;
   all must be present and the fit must say it fits with the margin the rules ask for. `MN_PLAN_ONLY` needs no allocation.
8. **First small run.** 1 node, `1e9` digits with write and compare against a reference, then 2 nodes (gates per TESTING.md §2), then a 1e10 run. **Give the first launch a long timeout (at least
   600 s, 2 x the cold figure of results/AAC7_PORT.md §ROCM: init 33 s warm-up there, an earlier first run did not finish the reciprocal in 240 s)**: ROCm 7.14 builds its code objects on first use.
   Watch `libamdhip64` in the log (the right ROCm), `-DXSHMEM_OFI` (path 1) or `-DXSHMEM_CRAY` (path 2) in the build log, `config-src:`. A second identical run shows the warm time; use that for any comparison.
9. **SHMEM NIC checks** (`SHMEM_*` lines of the facts; the probe prints its reading as "SHMEM NIC advice"):
   * `SHMEM_TWOPE_PROBE`: `shmem_ptr_peers_nonnull=1/1` means the library has an on-node path (XPMEM present: `XPMEM_DEV=1`); `0/1` means NULL (aac7 11.8: no `/dev/xpmem`).
     `get_ok`/`put_ok` must be 1. `SHMEM_TWOPE_LOCALITY_ONNODE_RC` != 0 (a crash) is the aac7 behaviour: not an error of the probe; rc 0 and `shmem_ptr` non-NULL means a faster on-node path exists to be measured.
   * `heap_is_device=1`: the library's `shmem_malloc` returns device memory, so the Cray backend's symmetric heap is device memory (otherwise ecalc falls back to the registered host pool and says so); `0` = host heap, the template's
   * `SHMEM_NICS{1,2,4,8}_RC` and `SHMEM_POLICY_*_RC`: which `SHMEM_OFI_NUM_NICS` the library accepts for ONE PE, and which NIC policies exist. Accepts 2 (and more): one PE can drive more than one NIC. Rejects
     everything above 1 ("only 1 NIC available"): one PE = one NIC, so with one PE per APU only one of the two NICs of each APU is used.
   Decision rule (assumed, to be confirmed by the user): path 1 (xshmem over libfabric, the default build) is the reference; path 2 (`XSHMEM=cray`, `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`, `README.md`) is adopted for the target run if it measures faster in an ABBA at the target (≤ 4 rounds, with an A/A control). The aac7 reference is `results/XSHMEM.md` (craylane): path 2 +7.5 % against path 1 at 3 n, 1e11 (the gap is init, +8 s).

Key table (every `__FILL__` of the template):

| key | determined by | expected |
|---|---|---|
| `TARGET_ROCM`, `TARGET_SMA_MODULE`, `TARGET_DSMML_MODULE` | step 1 (probe `PICK_*`) | module names whose versions are the stated ROCm, OpenSHMEM, and the newest dsmml |
| `MNRUN_MODULES` | step 1 | `"<dsmml> <openshmem> <rocm>"`, loads with rc 0 |
| `MNRUN_UNLOAD` | step 1 (`MOD_LOADED_ROCM`) | the default-loaded ROCm module(s) or empty |
| `GMP_HOME` | step 2 | a prefix with `include/gmp.h`, or the key deleted |
| `TARGET_PARTITION` | probe `SLURM_DEFAULT_PARTITION` / `PARTITION=` lines, then the site guide for the production queue | a name; node count of the partition >= 576 |
| `ECALC_OUT_DIR` | step 8 plus the `FS_CANDIDATE=` lines: a shared parallel FS (not tmpfs/RAM `/tmp`); free >= 0.417 B x digits (15.9 TB at 3.82e13); a 1-node `dd`/ecalc write test | a directory; single-file rate scales to ~1 TB/s at 32 writers per node (stated) |
| `MNACCEPT_TMP` | a non-RAM directory with room for the acceptance scratch (`TMP_FSTYPE` is the node `/tmp`'s type) | a directory on a disk or shared FS |
| `ECALC_NODE_GB` | step 5 | 450-500 (assumed for 512 GB nodes) |
| `MNRUN_EXCLUDE_HOSTS` | step 6 | empty at the start; the screen's `EXCLUDE` later |

Slurm limits (probe `QOS=`, `ASSOC=`, `CFG_*`): read the per-user node limit (`node=` in `MaxTRESPU`) against the 576-node job before any large submission; a pending job above the limit never starts (aac7: 6 nodes).
Huge pages and kernel memory: see "First SHMEM run on the target" below (it replaces the former one-line note; keep the probe's `SHM_MOUNT` line).

## First SHMEM run on the target

Background and evidence: `results/SHMEM.md` (cause of the aac7 stalls, the stall checklist and the target implications, condensed) and `archive:results_merged_2026-10-23/SHMEM_STALLS.md` (the full report: causes sections 4-5; target implications section 9; hugetlb switch 9.1; healthy values 8.3; stall checklist 8.4). The SHMEM transport depends on three things outside ecalc: kernel memory settings, the GPU driver's queue eviction
when the kernel moves pages, and the NIC count per node. The probe (node phase, `--hold`) reads them; none is changed by it.

| probe fact | meaning | switch choice |
|---|---|---|
| `KERNEL`, `KERNEL_GE_6_1`, `MADV_COLLAPSE` | `works` needs kernel >= 6.1 or a backport (the probe tests it on an anonymous mapping) | `works`: the node-process heap collapses the heaps to 2 MiB pages, the fix of the aac7 stalls. `errno=22` on an idle node (with `KERNEL_GE_6_1=no`, or the same on a repeat): unsupported; on a busy host it can also mean no free 2 MiB block. The run prints a collapse failure line and keeps 4 KiB heap pages, which are the pages compaction migrates (the stall cause): then use the hugetlb heap if a pool exists, else expect stalls and ask the site for a backport or `compaction_proactiveness=0` |
| `HUGETLB_2M_FREE`, `_NR`, `_PER_NUMA`, `_OVERCOMMIT` | the 2 MiB pool; per NUMA node `free/total` | pool present: reserve about heap size / 2 MiB per node-process (plus the library's own needs) and A/B the libsma heap on hugetlb (path 2, `TASKS.md` target session). No pool (aac7: 0, overcommit 0): the switch prints needed and free pages and falls back to `NP_HEAP_THP`; reserve at boot or in a job prolog, before memory fragments |
| `THP_enabled`, `THP_defrag`, `THP_shmem_enabled`, `THP_khugepaged_defrag` | aac7: `always`/`madvise`/`never`; AMD's MI300A guidance wants THP `always` for unified memory | `enabled=never` makes MADV_COLLAPSE fail: stop and ask the site. `shmem_enabled` other than `never` or a `huge=` on `/dev/shm` changes how the SHMEM heap file is backed: record it, re-run the 2-node gate |
| `KERNEL_MM_COMPACTION_PROACTIVENESS`, `_COMPACT_UNEVICTABLE_ALLOWED` | aac7 and AMD's guide: 20 and 1 | keep both for the first run. `0` is an A/B per job (stalls, KFD `evicted_ms`, GPU allocation time at start-up), not a default; only if stalls show up in the checklist below |
| `BUDDY_FRAG_SCORE`, `BUDDY_FREE_GE2M_MB` | per NUMA node and zone: share of free memory in blocks below 2 MiB (above 90 starts a proactive kcompactd pass); free MiB in blocks of 2 MiB or more | an idle node should be low (aac7 clean runs 1-17; start-up itself pushes node 0 of the first node to 91-99). A high score before the job: compact or drop caches in a prolog |
| `CXI_DEV_COUNT`, `FI_CXI_DOMAINS` | NICs per node (8 on the target, 4 on aac7) | `SHMEM_OFI_NUM_NICS` defaults to 1: set it to the NIC count with a NUMA or user mapping (the launcher caps it; the library aborts when asked for more NICs than exist). Steps 3, 6, 9 |
| `KFD_PRESENT` | the GPU driver's per-process statistics exist | needed for the `evicted_ms` column below |

Checklist for the first SHMEM run (after steps 0-9; 2 nodes, `1e10` digits or larger, then 4 nodes), adding to the launch line:

1. Read the log: the node-process heap line (`heap ... MiB PMD-mapped after`) shows whether the heap was collapsed to 2 MiB pages; a shortfall means 4 KiB pages are left: see the `MADV_COLLAPSE` row. (The communication trace and its report were removed with the plain SHMEM transport, Q103; the stall checklist of `results/SHMEM.md` uses `/proc/vmstat` deltas and the KFD counters.)
2. Stalls present: follow the checklist in `results/SHMEM.md` (per-second `/proc/vmstat` deltas on the late node, kcompactd ticks, score above 90, KFD `evicted_ms`), then try in this order: hugetlb heap if a pool exists; `compaction_proactiveness=0` per job (A/B with an A/A control, at least 4 ABBA pairs); a prolog that compacts and drops caches.
3. Measure the stall rate per node-hour on a short run before assuming it is zero: at 576 nodes one frozen node costs everyone (section 6.4 of the archived report).
4. Compare SHMEM (path 2) against libfabric (path 1), same digits and nodes, A/A first; the first launch keeps the long timeout of step 8.

### SHMEM settings to use on the target

Start with none of the stall switches on the launch line; the node-process heap collapse to 2 MiB pages is on by default (the stall fix).
- Measure first, in this order: (1) the collapse lines and the stall checklist above (hugetlb pool present: the libsma heap on hugetlb against off, 4 nodes, path 2); (2) SHMEM against libfabric at 2, 4 and 8 nodes with the same digits per node, to see whether the aac7 gap (parity at 1 peer, about +7 % at 3, about +9 % at 5) grows with the peers on the target fabric; (3) a repeat of the 2-node pair as a control.
