# ATRIM — BS_ARENA_TRIM_GB (trim the dm arena need)

**Question.** Does trimming the dm layout's arena need by a fixed number of GB per APU save node memory without costing time or remaps?
**Change / branch.** Branch `atrim`; switch `BS_ARENA_TRIM_GB`, default 0 (off). Subtracted at the end of `dm_layout`, clamped at `max(v2, v3)`; rank 0 prints the decision; twin `mem_model.py arena_trim`.
**Method.** aac7, `BS_ARENA_TRIM_GB=8` (TR8) against the switch off (S); 2 n at 1.2882e11 digits, 3 n at 1.9323e11 digits; `DB_POOL_VERBOSE=1` for remaps.
**Results.**

| size | arm | arena per node (GB) | time (s) | runs | remaps |
|---|---|---|---|---|---|
| 2 n 1.2882e11 | S | 266.3 (m) | 115.2 (m) | 7 | 0 |
| 2 n 1.2882e11 | TR8 | 231.9 (m), -34.4 | 111.4 (m) | 5 | 0 |
| 3 n 1.9323e11 | S | - | 149.4 (m) | - | 0 |
| 3 n 1.9323e11 | TR8 | - | 149.6 (m) | - | 0 |

**Verdict.** Needs more data: memory saving is large at 2 n and time is neutral (3 n) to slightly better (2 n); the digits gate (item 9lu_atrim_gate: 1 n and 2 n, SHMEM and libfabric) decides whether it stays opt-in.
**Open issues.** No adoption decision; gate result pending; no 4 n / 6 n data.
**RESUME.** Branch `atrim` merged with main eb0c5f0; next: collect 9lu_atrim_gate.
