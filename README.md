# ntt — ecalc: digits of e on AMD MI300A nodes

`ecalc` computes ⌊10^d · e⌋ in decimal. It uses binary splitting of Σ 1/k!, a Newton reciprocal and a two-half
division, on numbers held in device memory as base-10¹⁸ limbs (the limbs are the digits; no radix conversion). Products
are number-theoretic transforms modulo three or four primes, distributed over the four APUs of a node and over node
groups. The goal is **3.82 × 10¹³ digits on 576 nodes** of four MI300A APUs each (≈ 182 s without the write, modelled).
One node does 6.441 × 10¹⁰ digits in ≈ 76-78 s (measured on the aac7 test system).

## Layout

| path | what |
|---|---|
| `ecalc/` | the program (C/HIP), its `Makefile`, launch and test scripts (`mnrun.sh`, `mnaccept.sh`, `e16_headline.sh`, `target_kit.sh`, `digcmp.sh`), the models (`estimate.py`, `mn_model.py`, `mem_model.py`, `calib_table.py`), the switch registry (`switches.tsv`) and `README.md` (every switch) |
| `ecalc/tests/` | one GMP-checked test program per module (`t_ntt`, `t_mul`, `t_dbig`, `t_newton`, `t_mn_grid`, …) and helper scripts |
| `tools/` | `gate.sh` (build-and-digit gate), `rundriver.sh` (watchdog for driver scripts), `unpack_digits` (packed → ASCII), profile loader and checks, node screen, work-queue runner |
| `profiles/` | machine profiles: `aac7.conf`, `aac6.conf`, `target.conf.template` |
| `tests/` | two model scripts (seed-span model, block-pool replay) |
| `PLAN.md` | the design: algorithm, distribution, memory, verification, the plan to the target run |
| `RESULTS.md` | what is in force, with the evidence file of each adopted change |
| `TASKS.md` | open work, user decisions, target-session items |
| `results/` | agent reports (`<ID>.md`) cited by the code, `ecalc/README.md` and the files above |

History (the old plan and measurement log, older reports, moved drivers and raw logs) is kept outside the repository;
notes cite it as `archive:<path>`.

## Build

One node (aac6 or any ROCm system with GMP):

    cd ecalc && module load rocm && make

Multi-node (aac7; Cray OpenSHMEMX, GMP built from source):

    cd ecalc && source aac7env.sh && module load cray-dsmml cray-openshmemx
    make -B SHMEM_CRAY=1 GMP_HOME=~/gmp      # the build log must show -DCOMM_SHMEM

## Run

One node, inside an allocation of one node with 4 GPUs:

    ./ecalc 1000000000 /tmp/e9.out                  # 10^9 digits, packed output, prints VERIFY OK
    ./digcmp.sh /tmp/e9.out ref/e_1000000000.txt    # compare with a reference: "identical"
    ../tools/unpack_digits -o /tmp/e9.txt /tmp/e9.out

Multi-node, one process per node (the launch line of the test system is the `LINE` of `e16_headline.sh`):

    SLURM_JOB_ID=<hold> ./mnrun.sh <nodes> env <settings> ./ecalc <digits> [outfile]

`mnrun.sh` sets the communicator environment and the libfabric defaults. The target's launch line is
`profiles/target.conf.template`; `ecalc/target_kit.sh -h` describes the target session.

## Test

- `tools/gate.sh --build --nodes 1` (and `--nodes 2` for multi-node code): build, 10⁹ digits, VERIFY OK, digits identical.
  Every code change passes it before merge.
- `ecalc/mnaccept.sh <hold> [--only unit,e9,mn,grid,ckpt,sched,recheck,corr] [--full]`: the standing regression (unit
  tests, 10⁸-10⁹ digits at 1-4 processes, checkpoint restart, recheck, forced corrections); one PASS/FAIL line per step.

## Results and decisions

What is in force and its evidence: `RESULTS.md`. Every switch with its default and evidence: `ecalc/README.md`.
Open work and the user's decisions: `TASKS.md`. Each experiment writes `results/<ID>.md`.
