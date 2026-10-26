# TESTING — how to check a change to ecalc

Scope: the checks a change must pass, how to run them, the timing method, and the merge criteria. | Maintained: updated in
the same merge as any change to `tools/gate.sh`, `ecalc/mnaccept.sh`, the unit tests or the merge rules.

Cluster access, allocations and paths are in the private protocol; this file names only repository scripts. Where a
command needs a Slurm allocation it takes a job id `<hold>` of an allocation you own.

## 1. The rule

New behaviour goes behind an environment switch, **off by default**, and the digits must stay byte-identical: `VERIFY OK`
and a compare against reference digits. The user adopts a change on measured data. Every number you report is labelled
measured, modelled or assumed.

## 2. The gate (every change)

`tools/gate.sh` is the build-and-digit gate. It builds, runs 10⁹ digits, and passes only when the return code is 0,
`VERIFY OK` is printed on every node, the digits are identical to the reference, and nothing segfaulted.
Every size runs the production single-node flow (the device DKM division, `results/CODE_REDUCTION.md §SIMP`), so the one-node 10⁹ gate
covers it; its log shows the `I3:` line.

    tools/gate.sh --build --nodes 1 --hold <hold>          # one node
    tools/gate.sh --build --nodes 2 --hold <hold>          # two nodes: required when multi-node code changed
    tools/gate.sh --nodes 2 --alloc --line aac7            # own short allocation instead of a hold

**Path 2 gate (the Cray SHMEM path, user rule Q85; run when `xshmem*`, `comm_xshmem.c`, `pe_geo.c`, the launcher's P branch or shared multi-node code changed).**
Build as in `PLAN.md` section 3 (path 2). Then 10⁹ digits identical to the reference at 1 node and at 2 nodes with
`--env "ECALC_PE_PER_GPU=1 XSHMEM_LANES=2"` (the gate passes `--env` words to `mnrun.sh`), and `tests/t_xshmem correct` with `XSHMEM_LANES=2` at 1, 2 and 3 nodes
x 4 PEs (build `make XSHMEM=cray GMP_HOME=<gmp> tests/t_xshmem`; every PE must print `XT_RESULT PASS`). Path 1 is the plain gate above and must stay digit-identical and no
slower after any shared-code change. A timing claim between the paths needs interleaved pairs (ABBA) and an A/A control: aac7 run times are bimodal, so single runs prove nothing (`results/NODE_SLOWMODE.md §BM`, `results/XSHMEM.md` craylane). Digit reference at production size: 2 n 1e11 on both paths gives sha 578f5efb0ff2b9af of the part files' concatenated output (main after Q96, `results/XSHMEM.md §XWLAT`).

- `--build` runs `make -B` in `ecalc/` (path 1; `GATE_XSHMEM=cray` builds path 2; for a multi-node run the gate checks the
  build log for `-DXSHMEM_OFI` or `-DXSHMEM_CRAY` and refuses the run without it). It also runs `tools/switch_registry.py --check` and
  `tools/profile_load.py --validate-all`; either failing is a FAIL. The registry check's review-date `WARN` lines (a
  setting past its review date in `ecalc/switches_review.tsv`, or an experiment without one) are printed, never a FAIL.
- `--line` selects the launch line (the test system's is the `LAUNCH_LINE_KEYS` of `profiles/aac7.conf`, printed by
  `tools/profile_load.py aac7 --line`; the target's is supplied as a file); `--profile NAME` loads `profiles/NAME.conf`; `--env "K=V …"` adds settings; `--digits` and
  `--ref` change size and reference (only 10⁹ has a default reference).
- At `--nodes` > 1 every multi-node product runs the four-prime 24-digit form P24 (D18; `results/LAUNCH_DEFAULTS.md §DEF`): the run log must show `plane pool 0 for 4 primes`; a missing line is a FAIL, and the digits must be identical as always.
- **Memory fit (FIT, user decision D26, `results/LAUNCH_DEFAULTS.md §FIT`).** Before any node time is used (before `--alloc`'s sbatch and before
  the run in a hold) the gate prints the run's memory fit, `tools/plan_values.py --fit` on the launch line (`fit.txt`): the
  planner's device peak per node and per APU against the device limit `ECALC_DEVICE_GB` (the profile's measured edge; code
  default 373.44, the target's), the node against `ECALC_NODE_GB`, both margins, the largest consumers and the `DM_MN_WRAP`
  decision. A run that does not fit is a FAIL ("memory does not fit"), nothing is launched. `mnrun.sh` repeats the check
  before its srun (exit 4) and `ecalc` prints the same report as `memory fit:` lines in `run.log`. `GATE_FIT=0` /
  `MNRUN_FIT=0` skip it, for deliberate edge tests only.
- Output: `build.log`, `run.log` and one summary line `gate.txt` (`gate PASS | nodes … | digits … | total …s`). The digit
  files are deleted afterwards. The gate kills only its own processes and cancels only an allocation it submitted.
- Exit status 0 = PASS, 1 = FAIL. A build without the xshmem engine (`XSHMEM=0`) runs `--nodes 1` only.

## 3. The standing regression

`ecalc/mnaccept.sh <hold> [--full] [--only unit,grid,e9,mn,ckpt,recheck,corr]` runs on the login node against a
one-node allocation and prints one PASS/FAIL line per step, a summary, and exits with the number of failures. Logs go to
`ecalc/results/mnaccept/<hold>/`.
Durations on aac7 (measured 2026-10-13, results/CRASHES_HANGS.md §HANG): `--only unit` ≈ 25 min on a 2-node hold, `grid` ≈ 2 min,
`t_mul 20` ≈ 12 min. Give a queue item running `unit` a step timeout of at least 2400 s and `# EST_MIN=` of at least 45
(the runner stops an item at 1.5 × EST_MIN + 5 min); a shorter limit looks like a hang.

| step | what it checks |
|---|---|
| `unit` | 18 tests: `t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `t_verify`, `t_out`, `t_patch`, `t_arith` (`modarith`, `roots`, `primes gpu`), `t_crt`, `t_p24`, `t_crtk`, `t_strategy 20`, `t_seed`, `t_dist`, `t_mn_grid` at 2 processes |
| `grid` | `t_mn_grid` at 2 processes over xshmem |
| `e9` | 10⁹ digits at size 1 against the reference |
| `mn` | 10⁸ at sizes 2, 3, 4 and 10⁹ at sizes 2 and 4 (several node-processes on one node; the part files concatenated and compared) |
| `ckpt` | 10⁸ at size 2, the top-level set in `BS_CKPT_DIR`: written (one set per node, nothing else) and read by the recheck; dropped under the budget (`ECALC_CKPT_TOP=2`, an emulated slow disk); none without `ECALC_CKPT_TOP`; digits identical |
| `recheck` | `ECALC_RECHECK=1` on finished files: OK on intact files, FAILED on a copy with one digit flipped |
| `corr` | forced division corrections: the deferred patch rewrites digits inside the file; digits identical, RECHECK OK |
| `full` (`--full`) | the standard size at size 1 (`ECALC_STD_DIGITS`, default 10¹¹) against its reference, then RECHECK |

References are `ecalc/ref/e_<digits>.txt` of the clone, else `ECALC_REF`; make a missing one with `ref/gen_e` (GMP).

A recheck of a large run is bound by the file system, not the node: it reads every part file and the top-level set
again. On aac7 (NFS) that is ≈ 12.5 MB/s per node for the parts and ≈ 45 MB/s for the set (measured at 2 nodes,
results/CHECKPOINT.md §CKPT): 281 s at 10¹⁰ and 527 s at 2 × 10¹⁰ on 2 nodes (measured), ≈ 1 h at 3.86 × 10¹¹ on 6 nodes (modelled),
7380 s (2 h 3 min) there (measured, 2026-10-14).
Give it a timeout of at least 3 h for the 6-node large run (the model is about half the measured time); a recheck killed at its timeout leaves the
question open, and killed runs have been followed by nodes draining for a reboot.

## 4. Which checks for which change

| change touches | run |
|---|---|
| anything in `ecalc/` | gate, 1 node; plus 2 nodes for multi-node code (comm, `mn*`, `rns_dist.c`, `newton_db.c` sharded paths, output) |
| the engine (NTT, CRT, products, binary splitting, Newton, memory layout) | `mnaccept.sh <hold> --only unit,e9` plus the touched steps (`mn`, `grid`, `ckpt`, `recheck`, `corr`); `t_newton`, `t_mul`; 4 × 10¹⁰ digits identical to the reference; `ecalc/mem_model.py --check-c` exact if sizing changed |
| switches or defaults | `tools/switch_registry.py --check`; the switch row in `ecalc/README.md` |
| PE identity and process-per-GPU (`ecalc/vpe.{h,c}`, the `vpe_*` part of `mn.c`, `ECALC_PE_PER_GPU`) | `tests/t_vpe host` (login node is enough; checks the fatal paths in forked children); under `mnrun.sh` with `ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem` (4 tasks per node) `tests/t_vpe all` / `probe` at 1, 2, 3 nodes (one `T_VPE_RESULT PASS` per PE: 4, 8, 12), `t_vpe info` (launch and binding probe: `PPG_INFO` line per PE), `t_vpe die` and `t_vpe badlayout` (the step must end nonzero within 120 s, no task left); the flow's writers: `tests/t_out --ppg` (host only: one process per PE writes its part, parts / residues / windows vs the reference); the switch-off path must stay byte-identical (gate at 1 node, digits and log text); the form S product on lane 0 of the forced PE view: `tests/t_dist --vpe`, `tests/t_dbig --vpe` (no network). The thread mode (`ECALC_VPE`) and its tests were removed 2026-10-23 (Q86; branch `vpe-int`) |
| the xshmem layer (`xshmem*.{h,c}`, `comm_xshmem.c`, `pe_geo.c`) | build with the plain `make ecalc` (XSHMEM=ofi is the default where libfabric is found; `XSHMEM=0` for the old build); `tests/t_pe_geo` (host only; `t_pe_geo live` under `srun` is the one-hold boot + topology check, results/XSHMEM.md); `tests/t_xshmem` at 1, 2 and 3 nodes x 4 PEs (the default `correct` set: `basic exch deadlock layer limit tail`); then the gate at 1, 2 and 3 nodes with no transport variables (xshmem with `XSHMEM_ROUND_MB=128`; digits identical), and `mnaccept.sh <hold> --only mn` (default env) (`results/XSHMEM.md` §5, §6) |
| process layout P and lanes (`ECALC_PE_PER_GPU`, `XSHMEM_LANES`, `RNS_BATCH_SPLIT`, `xshmem_cray.h`, the launcher's P branch) | the path 2 gate (section 2) at 1 and 2 nodes, and D against P digits at 1, 2 and 3 nodes (`sha` of the output file) |
| speed claims | the timing method in section 6, on top of the above |

## 5. Unit tests and digit comparison

Local unit tests are `ecalc/tests/t_*.c`, built by `make` in `ecalc/` (`make tests/t_<name>`), one GMP-checked program per
module (`t_ntt`, `t_arith`, `t_crt`, `t_mul`, `t_dbig`, `t_bs`, `t_newton`, `t_dist`, `t_mn_grid`,
`t_p24`, `t_verify`, `t_out`, `t_patch`, `t_vpe`, …). Run one inside a one-node allocation with 4 GPUs (or `mnaccept.sh <hold> --only unit` for the standard set):

    srun --jobid=<hold> -N1 --gpus=4 --overlap ./tests/t_ntt 24            # arguments are the test's size
    srun --jobid=<hold> -N1 --gpus=4 --overlap ./ref/gen_e 1000000000 ref  # reference digits by GMP

`tests/harness.h` provides the assertions. `tests/tref.h` is the tests' reference product (`results/CODE_REDUCTION.md §TREF`): `tref_check` compares a product exactly with GMP up to 2^21 product limbs and above that by the residue form (limbs below B, length, residues modulo three 62-bit primes, the low 64 limbs exactly, the top limbs within exact bounds); `tref_faults` checks the reference itself (a changed limb must fail; `t_mul` runs it on every product). `tools/test_unpack.py` and `tests/t_out` test the packed format.
Python unit tests of the tools (`switch_registry.py`, `doccheck.py`, the models; stdlib, each < 5 s): `python3 -m unittest discover -s tests` from the repository root (`tests/test_tools.py`).

Digits against a reference:

    ecalc/digcmp.sh <outfile> <ASCII reference>          # prints identical / DIFFERS (packed or ASCII, any parts)
    tools/unpack_digits --cmp <reference> <parts…>       # same, without an ASCII copy; -n only checks residues

A run's own checks (`VERIFY OK`: T1 residues, T2 windows) are described in `ALGORITHM.md` §10; they do not replace the
reference compare, they replace it only where no reference exists.

## 6. Timing claims

One pair is never a result. Use **paired ABBA rounds**: in each round run A, B, B, A (A = baseline, B = change) in the same
allocation on the same nodes, so drift in the system averages out. Run at the size the claim is about, with digit output
off for multi-node timing runs (digits are written only by the gates).

- **Stop rule.** Stop at the first of: p < 0.05 on the paired differences; a 95 % confidence interval bound that decides
  the question (the whole interval above or below the size that matters); or the round cap, **at most 4 ABBA rounds**
  per question unless the coordinator allows more.
- **Report** the mean difference B − A with its 95 % CI and the number of pairs, never a bare ratio. Each arm's wall is the
  run's own total line; state the size, node count, and settings of both arms.
- A multi-node timing comparison is
  invalid when the rate shifted ("slow mode") — repeat the rounds.
- A long driver is one fire-and-forget script on the cluster with a done marker, not a polling agent.

## 7. Merge criteria

A branch merges to `main` when:

1. the gate passes: 1 node, and 2 nodes for multi-node code;
2. the checks of section 4 for the touched area pass, digits identical to the reference;
3. new behaviour is behind a switch, off by default, with its row in `ecalc/README.md` and the registry regenerated
   (`tools/switch_registry.py`), unless the user has adopted it as a default;
4. `results/<ID>.md` exists (section 8) and `RESULTS.md` has its short section (written by the coordinator unless the task
   says otherwise);
5. the docs the change touches are updated in the same merge (`ALGORITHM.md` for the algorithm, `ecalc/README.md` for
   switches, `PLAN.md` for status, `TASKS.md` for open work);
6. the commit passes the leak guard hooks (no host names, user names, paths, addresses, credentials, or private-doc
   contents in tracked files); never bypass them;
7. the session ends with the public remote, the local clone and the cluster clones on the same `main` commit.

## 8. Report template (`results/<ID>.md`)

    # <ID> — <title>
    **Question.** What is being decided or checked, in one or two sentences.
    **Change / branch.** Branch, commits, the switch name and default.
    **Method.** Exact commands, sizes (digits, nodes), settings of each arm, number of rounds, reference used.
    **Results.** A table: row per size or arm; columns with units; every number labelled (measured / modelled / assumed);
    timings as mean difference B − A with the 95 % CI and the pair count; digits: identical / not; VERIFY lines.
    **Verdict.** Adopt / do not adopt / needs more data, with the number that decides it.
    **Open issues.** What was not tested or is unexplained.
    **RESUME.** The state needed to continue: branch, last commit, what is done, the next command.

Raw logs stay on the cluster or in the local archive; the report cites their names, not their contents.

## 9. Documentation check

`tools/doccheck.py` (read-only, about 1 s) checks that the docs match the tree: the switch registry is current, the central
docs have a `Scope:` line, every cited `results/`, `tools/`, `ecalc/`, `tests/`, `profiles/` path and markdown link exists,
`PLAN §N` references name an existing section, no citation of the old paper outside `results/`, and archive paths are
written `archive:<path>` (not as a repository folder). The registry's review-date warnings print as `[review]` lines and
are not failures. Failures print as `file:line: [check] reason`; exit 1, or exit 0 with `--warn`. `tools/gate.sh` runs it
first: `GATE_DOCCHECK=strict` (default) fails the gate, `warn` prints the problems and goes on, `off` skips it.

