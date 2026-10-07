# S18KIT — `ecalc/target_kit.sh`: one script for the next target session (branch `s18-kit`)

The user has no time to read docs on the target. `ecalc/target_kit.sh` is self-contained (`-h` gives the full usage, time
estimates and the one file to send back), runs each stage behind its own flag and timeout, never stops the other stages when
one fails, and writes a single `$OUT/KIT_SUMMARY.txt` to send back — logs under `$OUT/logs/` are backup detail only. It never
launches with `oshrun`: every multi-process stage is Slurm-native `srun --ntasks … --ntasks-per-node=1` (`mnrun.sh` for the
ecalc runs, direct `srun` for `t_edge`/`t_comm`), and sets `FI_UNIVERSE_SIZE` (max(4096, 4 × ntasks)) and `FI_LOG_LEVEL=warn`
only where the caller has not already set them (the TGTBENCH2-proposed defaults, docs/TARGET.md §4, not yet adopted on the
main launch line — this script adopts them for its own probe runs only).

## What it answers

| stage | answers | WISHLIST item |
|---|---|---|
| `env` | hostname list, ROCm version, module list, `fi_info -p cxi` domain count, Slurm job geometry | §1.2 (partial), environment record |
| `build` | the binary exists on this target, built with target-generic module detection (no aac7 version hard-coded) | prerequisite for everything below |
| `edge` | the device-memory edge (`t_edge dev`/`vmm`, run alone, first), the init footprint (host RSS, per-APU device in-use vs driver-used), the VMM arena map rate per APU and per node, the single-node 1e10 wall | §2.1 (re-confirms A6), §2.2 (process overhead in our own terms, not the harness's), §3.1 |
| `a3` | injection bandwidth with `comm_ofi`, 1 NIC/APU vs 2 NICs/APU (`COMM_OFI_NICS`), GB/s per node | §1.3 (A3) |
| `a4` | the all-to-all rate at 2/8/64 nodes with `MN_COMM_MARK=1`, VERIFY, wall | §1.4 (A4) |

A3 and A4 are "still the top uncertainty" per results/TGTBENCH2.md §0; this kit is what gets them measured.

## Stages and expected runtime (wall clock, once the allocation is live)

| stage | nodes | expected | what it runs |
|---|---|---|---|
| `env` | 1 (probed) | < 1 min | hostname/date, `scontrol show job`, `fi_info -p cxi`, `module list`, ROCm version |
| `build` | 0 (login/any) | 3–10 min | `make SHMEM_CRAY=1` for `ecalc`, `tests/t_comm`, `tests/t_edge`, `tools/` |
| `edge` | 1, alone | 5–10 min | `t_edge dev`, `t_edge vmm` (sequential, not concurrent with anything else on the node), `ecalc 1e9` with `MEM_REPORT_DEVS=1`, `ecalc 1e10` |
| `a3` | 2 | 3–5 min | `t_comm --bw` with `COMM_OFI_NICS="0;1;2;3"` then `"0,4;1,5;2,6;3,7"` |
| `a4` | 2, 8, 64 (skips what the allocation can't hold) | 5–10 min per node count | `mnrun.sh <n> env COMM_TRANSPORT=shmem MN_COMM_MARK=1 ecalc <n * 1e10> <out>` |

Total for a full run with an allocation that holds 64 nodes: roughly 30–55 minutes. `--only`/`--skip` (comma lists of
`env,build,edge,a3,a4`) narrow it; `--nodes-a4` narrows the node counts.

## Validation done here (login node / local, no job touched)

- `bash -n ecalc/target_kit.sh`: OK.
- `shellcheck` not available in this environment; not run here (rehearsal on aac7 §below checks for it too).
- `./ecalc/target_kit.sh -h`: prints the full usage block.
- `./ecalc/target_kit.sh --dry-run --site aac7 --jobid 99999 --out /tmp/...`: every stage prints its commands (no execution),
  `KIT_SUMMARY.txt` gets one `DRY-RUN` block per stage. `--only env,a4 --nodes-a4 2,8,64 --jobid 12287` confirmed stage
  selection and the per-node-count `mnrun.sh` lines (digits = n × 1e10, `FI_UNIVERSE_SIZE=4096` for n ≤ 1024).

## The aac7 rehearsal (for the coordinator to run)

Per the task's constraints: **no `srun`/`sbatch` on aac7, never touch jobs 12287/12294** beyond `--dry-run`'s printed text
(which never executes anything). The rehearsal is `--dry-run` only, on the aac7 **login** node, from the `~/b7acct` clone,
fetching and checking out this branch:

    sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com '
      cd ~/b7acct &&
      git fetch origin &&
      git checkout s18-kit &&
      git pull --ff-only &&
      cd ecalc &&
      bash -n target_kit.sh && echo "bash -n OK" &&
      (command -v shellcheck >/dev/null 2>&1 && shellcheck -S warning target_kit.sh || echo "shellcheck not available") &&
      ./target_kit.sh --dry-run --site aac7 --jobid 12287 --out /tmp/s18kit_dry_2n --nodes-a4 2 &&
      ./target_kit.sh --dry-run --site aac7 --jobid 12287 --out /tmp/s18kit_dry_8n --nodes-a4 8 &&
      cat /tmp/s18kit_dry_2n/KIT_SUMMARY.txt &&
      cat /tmp/s18kit_dry_8n/KIT_SUMMARY.txt
    '

64 nodes is not available on aac7 (no 576-node-class allocation there); the dry-run's own node-count argument covers it
instead (`--nodes-a4 2,8,64` prints the 64-node `mnrun.sh` line unconditionally in `--dry-run` mode — a real run would skip
it if the allocation is smaller). `--jobid 12287` here only supplies the text the printed commands would carry; `--dry-run`
never calls `srun`, `sbatch` or `squeue` against it.

To rehearse inside the hold (the coordinator, when ready — NOT run by this report): the same two `--dry-run` lines, then,
inside `srun --jobid=12287 --overlap` (or `--jobid=12294`), a real `--site aac7` run with `--nodes-a4 2` (and separately `8`)
and `--only env,build,edge,a3,a4` as needed, pointed at a scratch `--out`.

## Open items

- The VMM map-rate unit (per APU-GB mapped by one APU, or per node-GB) is still not settled (TGTBENCH2 Q5); `edge`'s summary
  reports the per-APU figures as printed and flags the per-node figure as a naive sum, not a wall-clock rate, pending that
  answer.
- `a4`'s node counts (2/8/64) are configurable (`--nodes-a4`) but the script does not request dragonfly placement (packed vs
  spread, WISHLIST §1.4); that is a `--switches=1@<time>` Slurm option for the coordinator to add at the `mnrun.sh` call site
  if the admins confirm the spread syntax.
- `build`'s SMA/DSMML auto-detection (`module -t avail cray-openshmemx|cray-dsmml`, highest version) is still a blind
  newest-wins pick; `TARGET_SMA_MODULE`/`TARGET_DSMML_MODULE` override it by hand if that is ever wrong on the target.
  ROCm itself no longer picks blindly -- see "Module handling" below.

## Module handling

Two bugs, found by rehearsing on aac7 (results/V16.md) and by re-reading the script against `mnrun.sh`/`aac7env.sh`:

1. **ROCm choice.** `module -t avail rocm | sort -V | tail -1` picks the *newest* version, which on aac7 is
   `rocm/7.14.0` -- a version that fails to link ecalc (results/V16.md: 7.12.0/7.13.0/7.14.0 and `rocm-new`/10.0.0 all
   hit the same `-fPIC` device-link error in `ld.lld`; `rocm/7.2.4` is the newest that links). The target's ROCm is
   unknown ahead of time ("do your best with the libraries you have"), so `stage_build` now tries candidates in
   order and keeps the first that actually builds `ecalc` + `tests/t_comm` + `tests/t_edge` + `tools`:
   `$TARGET_ROCM` alone if the caller set it; else `rocm/7.2.4`, `rocm/7.2.3`, `rocm/7.0.3` (whichever are available,
   in that order), then any other `rocm/<version>` module, newest first. `make clean` runs between attempts (stale
   `.o` from a different ROCm must not be reused); every attempt is logged, and the summary names the ROCm used and
   any that failed first. A versioned `rocm` module can conflict with a default-loaded `rocm` (aac7:
   `rocm/7.0.3`, the same trap V16 found) -- the build unloads whatever `rocm/*` the login shell already has before
   loading each candidate.
2. **Runtime stages never loaded modules.** Only the `build` subshell called `module load`; the `srun` stages for
   `edge`/`a3`/`a4` ran with whatever the caller's login shell had, which can be a different ROCm/SHMEM than the one
   that built the binaries. `stage_build` now writes the chosen set to `$OUT/kit_modules.env`
   (`TARGET_ROCM`/`TARGET_SMA_MODULE`/`TARGET_DSMML_MODULE`/`TARGET_ROCM_UNLOAD`); every later run stage
   (`edge`, `a3`, `a4` -- not `env`, which deliberately records the target's as-found default) sources it and fails
   with a clear message if it is missing (so `--only edge` etc. in a later invocation needs a prior `build` in the
   same `--out`), loads the same modules inside its own `srun` (`module_preamble()`, the unload-then-load convention
   `aac7env.sh`/`mnrun.sh` already use), and records the runtime ROCm actually seen on the compute node
   (`hipconfig --version`, inside the `srun`) in its log and the summary. `a4` additionally exports
   `MNRUN_MODULES`/`MNRUN_UNLOAD` to `mnrun.sh` from the same chosen set (honouring any caller override, per
   `mnrun.sh`'s own convention).

Validated on the aac7 login node (branch `s18-kit`, no `srun`/`sbatch`, jobs 12287/12294 untouched): `bash -n` OK;
`shellcheck` not installed there either. `--dry-run --site aac7 --jobid 12287 --nodes-a4 2` and `...--nodes-a4 8`
both print the ROCm candidate list (`rocm/7.2.4 rocm/7.2.3 rocm/7.0.3 rocm/7.14.0 rocm/7.13.0 rocm/7.12.0` on aac7)
and assume the first, write `kit_modules.env`, and every later-stage `srun` line carries the module unload/load
preamble and a `hipconfig --version` probe. A real `--only build` run (no `srun`, so safe on the login node) chose
`rocm/7.2.4` on the first try and built `ecalc`, `tests/t_comm`, `tests/t_edge`, and `tools` successfully --
confirming the candidate order matches results/V16.md's finding without needing the aac7-specific version hard-coded.
