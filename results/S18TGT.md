# S18TGT — target size 3.71e13 and the launch-flag defaults (branch `s18-target`, 2026-10-06)

Times Eastern. Labels: **(m)** measured, **(mod)** modelled, **(a)** assumed. Branch created from `origin/main` 7efeeb9,
pushed, not merged.

## Part 1 — target 3.71 × 10¹³

- `ecalc/mem_model.py`: `TARGET_DIGITS` 4.08e13 → **3.71e13** (the user's decision, 2026-10-06 ≈19:30 EDT: "3.71×10^13 is
  fine"); `TARGET_BELOW` 3.99e13 → **3.6e13**.
- Verified on aac7's login node (b7-vslot aa86cac, `MN_PLAN_ONLY=37100000000000:576` / `BS_LAYOUT_ONLY`, the TGT17 launch
  env + `COMM_OFI_PLAN_CXI=1`), not copied from TGTBENCH2.md blindly:
  - device_with (v-slots) **363.526347904 GB**, 9.914 GB under the 373.44 GB measured edge (m, target).
  - node / node_with v-slots **378.785025920 / 405.611794304 GB** (480 GB budget: +74.39 GB margin with v-slots).
  - pieces (node 0 / critical path) **103 / 107**, 1236 products, `plan check` OK.
  - `estimate.py --fabric target-m --D 64409722222.222 --bw 47`: **221.9 s** modelled without the output write / 225.1 s
    with it — matches results/TGTBENCH2.md §4 exactly.
- `TARGET_BELOW = 3.6e13`: same 206.158 GB arena tier as 3.71e13 (unchanged from 3.5–3.7104e13); pieces 101/107 (one
  node-0 piece-step below 103/107, the step is at 3.616–3.618e13). The next arena tier down (201.863 GB) starts at
  3.52–3.53e13 (94/102 pieces) — not used, since it is a different tier (the task asked for "pieces/arena differ", which
  the node-0 step already satisfies within the same tier, matching the style of the former TARGET_BELOW=3.99e13, also
  same-tier).
- `python3 mem_model.py --check-c /tmp/layout_371e13.txt` (same environment as the layout run): **exact, 0 terms not
  exact**, largest arena difference 0.0000 %.
- `estimate.py --target` runs cleanly; its "one step below" row now reflects 3.6e13 (175.2 s modelled without the write).
- `ecalc/e16_headline.sh`: `E16_DIGITS`'s default and comment re-measured at the new per-node share (3.71e13 / 576 →
  **6.441e10** digits/node, from `MN_PLAN_ONLY=37100000000000:576`'s P = 2061111111115 limbs, ceil(/576) = 3578317902
  limbs = 64409722236 digits, rounded to 1e6). `E16_DIGITS` at g=12: 772920000000 (pieces 118/118, 100 products, node
  359.46 / 370.93 GB with v-slots); at g=10: 644100000000 (pieces 95/95, 96 products, node 368.05 / 381.82 GB). Also
  updated `E16_BELOW` (760000000000 @ g=12, 630000000000 @ g=10) — not explicitly named in the task but left at the old
  values it would have been **above** the new `E16_DIGITS`, which is a real bug; fixed as a minimal, necessary, commented
  edit in the same file.

**Docs still quoting 4.08 × 10¹³ as the CURRENT target (not changed — left to the docs pass named in the task):**
internal target notes (§1's TGT17 paragraph, §4's `srun` line `ecalc 40800000000000` and its closing summary paragraph),
internal target task list (top entry), internal code notes (ME24 row), internal code notes,
`ecalc/README.md` (the launch-line banner, Batch 3's "target-is" line), `ecalc/estimate.py` (`--target`'s help text and
the `partial_row` history tuple naming "4.08e13 since TGT17"), `RESULTS.md` §110–112 (history, left as written). None of
these are code/script sizing logic — they are prose or help-text mentions, so left untouched per the task's scope.

## Part 2 — launch flags

`ecalc/mnrun.sh`'s SHMEM path now exports, only when unset, `FI_UNIVERSE_SIZE=max(4096, 4 × ntasks)` and
`FI_LOG_LEVEL=warn` (the TGTBENCH2 proposal, results/TGTBENCH2.md §1 L2 / §4; the user: "use the flags that optimize
performance but allow us to adjust to a different system later"). The symmetric heap is unchanged — mnrun.sh already
derives it from the `MN_PLAN_ONLY` plan (`SHMEM_SYMMETRIC_SIZE` and, for Cray, `XT_SYMMETRIC_HEAP_SIZE`, both pool + 512
MiB, each only-if-unset), so no new pairing was needed there; verified by reading the sos/cray branches side by side.
Chosen `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL` and the heap size are now printed in mnrun.sh's launch echo. internal target notes
§4's "PROPOSAL — NOT ADOPTED" block is now written as adopted, with the override-per-system idea in one line per
variable; `ecalc/README.md` gets two new switch-table rows. No C default changed (only `mnrun.sh`, a shell script).

`bash -n` passed on both `mnrun.sh` and `e16_headline.sh`.

## Part 3 — RESULTS.md

Appended §113 (B7ACCT: the v-slot accounting, formula, check-c exact, `ECALC_VSLOT_BUDGET` off by default, the
then-current target's margins), §114 (B7V17's hardware verification on aac7 job 12287, the coordinator's verbatim
measured figures at 3/6/10 nodes — every peak under its node's layout by 10–29 GB), §115 (TGTBENCH2's item-by-item
assessment and the three decision groups: target size, launch flags adopted, rejected items, and the unknowns left to
this project's own target kit) — same style as §108–112, after §112, nothing else in RESULTS.md touched.

## Open items / not done here

- The docs pass listed above (TARGET.md, TARGET_TASKS.md, README.md banners, estimate.py help text, the decision
  register and evaluation docs) still names 4.08 × 10¹³ as current.
- `ecalc/e16_headline.sh`'s 10/12-node figures are a separately measured scaled slice, re-measured here at the new
  target's share but not actually re-run on hardware (login-node plan/layout only, as instructed — no srun/sbatch).
- `ECALC_VSLOT_BUDGET` stays off (not evaluated for turning on at 3.71e13 specifically, though B7ACCT found no sizes
  where it changes a decision).

## RESUME

Complete. Branch `s18-target` (origin, 3 commits: 45c4d49 Part 1, 1e9e226 Part 2, d42a317 Part 3) pushed, not merged.
Nothing armed, no jobs running, no compute-node work was done (login node only, per the task's restriction).
