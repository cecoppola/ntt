# ntt — ecalc: digits of e on AMD MI300A nodes

`ecalc` computes ⌊10^d · e⌋ in decimal with binary splitting and a Newton division on device-resident numbers, using a four-APU
distributed number-theoretic transform. One node does 6.441 × 10¹⁰ digits in ≈ 78 s (measured, 2026-10-10); the goal is
3.71 × 10¹³ digits on 576 nodes (modelled: ≈ 180 s without the write, device 351.7 of 373.44 GB per node; the target itself is never
accessed). Objective and success measures: `docs/GOALS.md`.

| to find | read |
|---|---|
| the index of every document | `docs/README.md` |
| the algorithm, the defaults, the launch lines | `docs/code/00_OVERVIEW.md` |
| every environment switch, with its default and evidence | `ecalc/README.md` |
| what is open | `TASKS.md` |
| the 576-node runbook and estimate | `docs/TARGET.md` (tasks: `docs/TARGET_TASKS.md`) |
| every decision and its status | `docs/code/05_DECISION_REGISTER.md` |
| the measurement log | `RESULTS.md` (agent reports in `results/`) |
| how work agents operate (clusters, nodes, gates) | `docs/AGENT_PROTOCOL.md` |
| moved drivers, old docs and raw logs | `archive/MANIFEST.md` |

Layout: `ecalc/` the program, models and launch scripts (`make`; see its README); `tools/` the watchdog driver library and the digit
unpacker; `bench/` (with the top-level `suite`, `run`, `diff`, `isa.py`) the 2026-09 microbenchmark suite; `tests/` plan and gate notes of Phase 15; `results/` the reports; `archive/` history. Build and run
recipes: `ecalc/README.md`.
