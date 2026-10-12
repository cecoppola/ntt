# profiles/ -- machine profiles (draft, step one)

Status (O3, 2026-10-12): step 2 on branch `o3`, not merged. `tools/profile_load.py` validates and loads a profile (explicit environment > profile > code
default); `tools/with_profile.sh NAME -- cmd` runs a command under it and prints a `config-src:` line; `tools/gate.sh --profile NAME` uses it, and
`gate.sh --build` runs `switch_registry.py --check` and `profile_load.py --validate-all` first. Nothing reads a profile unless asked. Details: results/O3.md.

## Plan (agreed 2026-10-11)

Adopted settings become built-in defaults in the code. What stays machine-specific lives in a named profile here. One entry
point (later step) loads, in this order, **built-in defaults < profile < overrides** (the caller's environment / command line),
validates, and prints the effective configuration and its hash. Size-dependent values are never typed: the plan stage computes them.

## Files

| file | machine | status |
|---|---|---|
| `aac7.conf` | aac7 (Cray EX, 4 x MI300A, Slingshot-11, Cray OpenSHMEMX) | filled from `ecalc/aac7env.sh`, `mnrun.sh`, `e16_headline.sh`, the README launch table |
| `aac6.conf` | aac6 (MI300A, InfiniBand, OSHMEM, single node standard) | filled, small: aac6 is a correctness machine |
| `target.conf.template` | the production machine | every unknown is `__FILL__` with "how to determine"; copy to `target.conf` (not tracked: add it to `.git/info/exclude`) |

Site specifics of the target (names, module names, paths, node lists) must never be committed here. The tracked template holds
generic values only; the filled copy stays out of git.

## Format

* One `KEY=value` per line, shell-sourceable (`. profile.conf` must work). No `export`, no command substitution, no logic.
* Value with spaces: double quotes (`MNRUN_MODULES="a b c"`). `$HOME` / `${HOME}` may appear in path values; the loader expands
  them (`tools/gate.sh --line file:` does not, so keep path keys out of any file given to it).
* `#` starts a comment, also after a value. Section headers are comment lines of the form `# ---- [name] ----`; the section names are
  `modules/build`, `scheduler`, `paths`, `transport`, `libfabric`, `memory pools tied to the hardware`, `exchange tuning`,
  `adopted ... pending move to built-in defaults`, `safety/diagnostics`, `host exclusions`.
* `__FILL__` is the value of an unknown. The validator must refuse a profile that still has one (the checker lists them as UNFILLED).
* A key to be **unset** is not expressible yet (the target's record run unsets `ECALC_CHECKPOINT ECALC_CKPT_TOP BS_CKPT_DIR`): the
  entry point needs an `unset` section; open question in `results/C01.md`.
* Each key carries its evidence in the trailing comment: the README switch row, `results/<ID>.md`, or the user's decision with its
  date. Numbers are labelled (m) measured, (mod) modelled, (a) assumed. A line without evidence is a bug in the profile.

## What belongs where

| goes to | rule | examples |
|---|---|---|
| **profile** | a fact about the machine or site that no measurement on another machine would change: modules, SHMEM implementation, transport choice, NIC/fabric settings, memory pools bounded by the hardware, scheduler partition, scratch and output paths, excluded hosts, per-site safety limits | `MNRUN_MODULES`, `COMM_TRANSPORT`, `COMM_SHMEM_DEVHEAP`, `FI_MR_CACHE_MONITOR`, `MNRUN_EXCLUDE_HOSTS`, `MNACCEPT_TMP` |
| **built-in default** (code) | a choice the user adopted that is right on every supported machine, or a safe value of a tunable | `ECALC_NP=auto`, `RNS_DIST_CACHE_FIT=1`, `MN_T_CHUNK_MB`, `COMM_LAYER_*` once adopted everywhere |
| **computed** (plan stage) | anything that is a function of the digit count, node count and hardware sizes | `COMM_SHMEM_POOL_MB`, `SHMEM_SYMMETRIC_SIZE`, `XT_SYMMETRIC_HEAP_SIZE` (pool + 512 MiB), `COMM_OFI_POOL_MB`, `FI_UNIVERSE_SIZE` (max(4096, 4 x tasks)), `MN_GROUPS` at a given size |
| **override** | one run's deliberate deviation (an A/B arm, a test) | `E16_EXTRA`, `--env` of `tools/gate.sh`, the caller's environment |

Transitional section: today several adopted switches (T2048, X2, `ECALC_NP`, ...) are on the launch lines but not yet code defaults.
They are listed in the profile under `adopted ... pending move to built-in defaults` so that nothing is lost; as each becomes a code
default its line is deleted from every profile (the checker then shows it as PROFILE-ONLY equal to the default, the cue to delete it).
Switches slated for removal (tiers A and B of `results/A11.md`) are never put in a profile; the checker reports them as REMOVAL-SLATED.

## Checking a profile

```
tools/profile_check.sh profiles/aac7.conf          # compare with the aac7 launch lines (e16_headline.sh LINE, aac7env.sh exports)
tools/profile_check.sh profiles/aac6.conf -q       # problems only; --launch=aac7 forces the aac7 comparison, --strict exits 1 on MISMATCH/UNKNOWN
```

Statuses: MATCH (a launch line sets the same value), MISMATCH (it sets a different one), PROFILE-ONLY (no launch line sets it; the
script's own `${VAR:-default}` is shown for information), UNFILLED, UNKNOWN (no such name anywhere in `ecalc/` or `tools/`: a typo or a
key without a consumer), SCRIPT-ONLY (a launch line sets it but the profile lacks it), REMOVAL-SLATED. Read-only; it needs bash and grep.
