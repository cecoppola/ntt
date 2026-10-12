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

    tools/gate.sh --build --nodes 1 --hold <hold>          # one node
    tools/gate.sh --build --nodes 2 --hold <hold>          # two nodes: required when multi-node code changed
    tools/gate.sh --nodes 2 --alloc --line aac7            # own short allocation instead of a hold

- `--build` runs `make -B` in `ecalc/`. A multi-node build needs SHMEM (`SHMEM_CRAY=1`); the gate checks the build log
  for `-DCOMM_SHMEM` and refuses a multi-node run without it. It also runs `tools/switch_registry.py --check` and
  `tools/profile_load.py --validate-all`; either failing is a FAIL.
- `--line` selects the launch line (the test system's is parsed from `LINE` in `ecalc/e16_headline.sh`; the target's is
  supplied as a file); `--profile NAME` loads `profiles/NAME.conf`; `--env "K=V …"` adds settings; `--digits` and
  `--ref` change size and reference (only 10⁹ has a default reference).
- Output: `build.log`, `run.log` and one summary line `gate.txt` (`gate PASS | nodes … | digits … | total …s`). The digit
  files are deleted afterwards. The gate kills only its own processes and cancels only an allocation it submitted.
- Exit status 0 = PASS, 1 = FAIL. A single-node system without SHMEM runs `--nodes 1` only.

## 3. The standing regression

`ecalc/mnaccept.sh <hold> [--full] [--only unit,grid,e9,mn,ckpt,sched,recheck,corr]` runs on the login node against a
one-node allocation and prints one PASS/FAIL line per step, a summary, and exits with the number of failures. Logs go to
`ecalc/results/mnaccept/<hold>/`.

| step | what it checks |
|---|---|
| `unit` | the unit tests below (`t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `t_verify`, `t_out`, `t_patch`, `t_mn_grid` at 2 processes) |
| `grid` | `t_mn_grid` over SHMEM |
| `e9` | 10⁹ digits at size 1 against the reference |
| `mn` | 10⁸ at sizes 2, 3, 4 and 10⁹ at sizes 2 and 4 (several node-processes on one node; the part files concatenated and compared) |
| `ckpt` | 10⁸ at size 2: abort during binary splitting, restart, digits identical |
| `sched` | a tree-level restart under another schedule or node count is refused; under the same schedule it is identical |
| `recheck` | `ECALC_RECHECK=1` on finished files: OK on intact files, FAILED on a copy with one digit flipped |
| `corr` | forced division corrections: the deferred patch rewrites digits inside the file; digits identical, RECHECK OK |
| `full` (`--full`) | the standard size at size 1 (`ECALC_STD_DIGITS`, default 10¹¹) against its reference, then RECHECK |

References are `ecalc/ref/e_<digits>.txt` of the clone, else `ECALC_REF`; make a missing one with `ref/gen_e` (GMP).

## 4. Which checks for which change

| change touches | run |
|---|---|
| anything in `ecalc/` | gate, 1 node; plus 2 nodes for multi-node code (comm, `mn*`, `rns_dist.c`, `newton_db.c` sharded paths, output) |
| the engine (NTT, CRT, products, binary splitting, Newton, memory layout) | `mnaccept.sh <hold> --only unit,e9` plus the touched steps (`mn`, `grid`, `ckpt`, `sched`, `recheck`, `corr`); `t_newton`, `t_mul`; 4 × 10¹⁰ digits identical to the reference; `ecalc/mem_model.py --check-c` exact if sizing changed |
| switches or defaults | `tools/switch_registry.py --check`; the switch row in `ecalc/README.md` |
| speed claims | the timing method in section 6, on top of the above |

## 5. Unit tests and digit comparison

Local unit tests are `ecalc/tests/t_*.c`, built by `make` in `ecalc/` (`make tests/t_<name>`), one GMP-checked program per
module (`t_ntt`, `t_ntt3`, `t_modarith`, `t_primes`, `t_crt`, `t_mul`, `t_dbig`, `t_bs`, `t_newton`, `t_dist`, `t_mn_grid`,
`t_p24`, `t_verify`, `t_out`, `t_patch`, `t_spill`, `t_comm`, …). Run one inside a one-node allocation with 4 GPUs:

    ./run tests/t_ntt 24            # arguments are the test's size
    ./run ref/gen_e 1000000000 ref  # reference digits by GMP

`tests/harness.h` provides the assertions. `tests/` at the repository root holds two model scripts
(`t2_seed_model.py`, `as_pool_replay.py`); `tools/test_unpack.py` and `tools/test_pack60.c` test the packed format.

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
- `ecalc/calib_table.py <dirs…> --ab` turns run logs into rows with a rate-adjusted paired A/B and warns about a shift in
  the fabric rate between arms; `tools/rundriver.sh` is the watchdog for driver scripts. A multi-node timing comparison is
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
