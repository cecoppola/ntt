# ntt — ecalc: digits of e on AMD MI300A nodes

Scope: what the project is, the repository layout, the shortest build/run/test commands, and the map of the documents. |
Maintained: updated in the same merge as any change to the layout, the entry commands or the document set.

`ecalc` computes ⌊10^d · e⌋ in decimal. It splits the series Σ 1/k! by binary splitting, takes a Newton reciprocal and
divides in two halves, on numbers held in device memory as base-10¹⁸ limbs (the limbs are the digits; no radix
conversion). Products are number-theoretic transforms modulo three or four primes, distributed over the four APUs of a
node and over node groups. The goal is 3.82 × 10¹³ digits on 576 nodes; the current status is in `PLAN.md`.

## Documentation

| file | holds |
|---|---|
| `ALGORITHM.md` | how ecalc works: series, products, division, distribution, memory, output, verification |
| `PLAN.md` | the goal, the current status, launch lines, the road to the target run |
| `TESTING.md` | how to check a change: gate, regression, unit tests, timing method, merge criteria, report template |
| `ecalc/README.md` | every environment switch with its default and evidence; the files of `ecalc/` |
| `RESULTS.md` | what is in force, with the evidence file of each adopted change |
| `TASKS.md` | open work (by priority class), proposed items, decisions still acting, target-session items |
| `OPPORTUNITIES.md` | every performance idea (time, memory, concurrency, algorithm), open or tried, with its evidence |
| `results/` | measurement reports by topic, cited by the files above (index: `RESULTS.md`) |

History (the old plan, measurement logs, older reports, moved drivers) is kept outside the repository; notes cite it as
`archive:<path>`.

## Layout

| path | what |
|---|---|
| `ecalc/` | the program (C/HIP), its `Makefile`, launch and test scripts, the time and memory models, the switch registry |
| `ecalc/tests/` | one GMP-checked test program per module (`t_ntt`, `t_mul`, `t_dbig`, `t_newton`, `t_mn_grid`, …) and the test harness headers |
| `tools/` | `gate.sh`, `unpack_digits`, the profile loader and checks, node screen, `target_probe.sh` (target bring-up facts, profiles/README.md runbook) |
| `profiles/` | machine profiles: `aac7.conf`, `target.conf.template` |
| `tests/` | Python unit tests of the tools (`test_tools.py`) |

## Build and run

Needs ROCm (`module load rocm`) and GMP. One node:

    cd ecalc && make
    ./ecalc 1000000000 /tmp/e9.out                  # 10^9 digits, packed output, prints VERIFY OK
    ./digcmp.sh /tmp/e9.out ref/e_1000000000.txt    # compare with a reference: "identical"
    ../tools/unpack_digits -o /tmp/e9.txt /tmp/e9.out    # packed -> ASCII

Multi-node, one launch per allocation of `<nodes>` nodes (`SLURM_JOB_ID=<hold>` names the allocation):

    SLURM_JOB_ID=<hold> ./mnrun.sh <nodes> env <settings> ./ecalc <digits> [outfile]

**Two supported multi-node paths (user rule Q85, 2026-10-23): both stay working; a change to shared code must leave
path 1 digit-identical and no slower.** Current figures are in `PLAN.md` section 2.

1. **Libfabric** (default build). Build `make -C ecalc SHMEM_CRAY=1 GMP_HOME=<gmp>` (default `XSHMEM=ofi`: libfabric and the
   PMI2 header must be found; `tools/gate.sh` requires `-DCOMM_SHMEM` in the build log of a multi-node build).
   Run `./mnrun.sh <nodes> env ./ecalc <digits>`: layout D (one process per node, four APU threads), `COMM_TRANSPORT=xshmem`,
   no other setting.
2. **Cray SHMEM** (the target shape). Build `make -B -C ecalc XSHMEM=cray SHMEM_CRAY=1 GMP_HOME=<gmp>` (aac7: `module load
   cray-dsmml cray-openshmemx`; rebuild with `-B` when switching `XSHMEM`).
   Run `./mnrun.sh <nodes> env ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ./ecalc <digits>`: one process per GPU, two lanes.
   `mnrun.sh` adds, for a Cray build under `ECALC_PE_PER_GPU=1`, the NUMA placement (`MNRUN_PPG_CPUBIND=ldom`,
   `XSHMEM_CRAY_NUMA=1`), the automatic libsma heap and the plan. Digits are identical to path 1.

The exchange worker (`ECALC_XEX_THREAD=1`) is the default in every layout; `RNS_BATCH_SPLIT` is automatic under `ECALC_PE_PER_GPU=1`. The plain SHMEM transport and `COMM_TRACE` were removed 2026-10-25 (Q103). `mnrun.sh` sets the communicator environment; the
launch lines (test system and target) are in `PLAN.md` section 3; `ecalc/target_kit.sh -h` describes the target session.
Cluster access is described in the private protocol.

## Test

    tools/gate.sh --build --nodes 1 --hold <hold>      # every change; add --nodes 2 for multi-node code
    ecalc/mnaccept.sh <hold> --only unit,e9            # the standing regression (more steps: TESTING.md)

Every change passes the gate before merge. The full list of checks, the timing method and the merge criteria are in
`TESTING.md`.
