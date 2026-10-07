# S18DOCS — the one-page docs refresh for the project owner (branch `s18-docs`, 2026-10-06)

Docs only, no code changes (one exception: `ecalc/estimate.py`'s help text and two label strings are prose, not
logic — confirmed with `ast.parse`). Branch from `origin/main` 63f50ca, pushed, not merged. Labels: **(m)** measured,
**(mod)** modelled, **(a)** assumed.

## What changed

### docs/code/00_OVERVIEW.md
- Added a **"Defaults chosen for you"** box near the top: target size (3.71 × 10¹³), transport (`comm_ofi`), launch
  flags (`FI_UNIVERSE_SIZE`/`FI_LOG_LEVEL`), ROCm stance (validated against 7.2.4/7.0.3, target version unknown, the
  target kit builds with the best validated one available), the memory bar, and what's still unmeasured (A3
  injection, A4 scaling).
- Header "code state" updated from `5ae3278 (2026-10-05)` to `1e9e226 (2026-10-06)` with a one-line summary of what
  changed since.
- §1.1 ("what is computed"): target digit count 5.276e13/5.167e13 → 3.71e13, with the TGT17 (4.08e13) intermediate
  noted as history.
- §1.3 (standing estimate): new headline at 3.71e13 (221.9/225.1 s modelled, device 363.53 GB, node 405.61 GB with
  v-slots) with the old 5.276e13 figures kept explicitly as history (no fresh by-phase breakdown exists at the new
  target — flagged as an open item, not fabricated).
- §2 (map): added **07_COMM_OFI** as a row; added the OFIMEM/B7ACCT/TGTBENCH2 results files to "Other records"; noted
  RESULTS.md now runs to §115.
- §3 (defaults and launch line): replaced the entire launch-line code block with the current one (`COMM_OFI=1`,
  `DM_MN_LEAN=1`, `COMM_SHMEM_POOL_MB=1536`, `SHMEM_SYMMETRIC_HEAP_SIZE=2048M`, digit arg `37100000000000`); added the
  `FI_UNIVERSE_SIZE`/`FI_LOG_LEVEL` note; added a 2026-10-06 decisions line (`COMM_OFI` default, `ECALC_VSLOT_BUDGET`).
- §5 ("how not to get lost" / superseded-records table): fixed the `results/RUN16.md` row (it was marked "a skeleton,
  no results yet" — RESULTS §105 shows it is complete and measured); fixed the `estimate.py`/`mn_model.py` docstrings
  row and the closing "Label discipline" paragraph (both still claimed only newbench1's two numbers; added
  TGTBENCH2's measurements and the still-open A3/A4 risk).
- Glossary kept unchanged (it was not stale).

### docs/TARGET.md
- §1: added a new CURRENT paragraph for 3.71 × 10¹³ (TGTBENCH2/s18-target), demoted the TGT17 (4.08e13) paragraph to
  explicit history with a one-line explanation of *why* it was superseded (B7ACCT's v-slots left only 0.88 GB of
  margin, not the "comfortable" margin TGT17 reported before v-slots were known).
- §4: launch line's digit argument `40800000000000` → `37100000000000`; updated the trailing comment's history chain.
  (The `FI_UNIVERSE_SIZE`/`FI_LOG_LEVEL` "ADOPTED" block was already written as adopted by a prior s18-target commit —
  left as is.)
- The paragraph after the launch line (the "argument is the total digit count" note): rewritten for 3.71e13 as
  current, 4.08e13/5.276e13/5.167e13/5.1e13/4.25e13 kept as history in order.
- Checked §4 step 3(c) ("three parts") and the trap list: **already correct** — a prior agent had fixed step 3(c);
  left untouched per "don't rewrite history."

### docs/TARGET_TASKS.md
- "Where things stand" section: added a new top bullet for the 3.71e13 decision (CURRENT), demoted the TGT17 bullet
  to explicit history.
- T4 ("the headline run"): rewritten for 3.71e13 (modelled wall, device/node GB, pieces); 4.08e13 moved into its
  *History* sentence alongside 5.276e13/5.167e13/5.1e13/4.25e13.
- T4b/T4c/T5 untouched: they describe the then-current target's own measurements and are explicitly historical by
  design (T4c is literally "the runtime one step below, *history*: at the then-current 5.1e13 target").

### docs/code/05_DECISION_REGISTER.md
- ME24 (reject the harness's managed-memory host spill): status **PROPOSED → REJECTED** (RESULTS §115 confirms the
  user rejected it); its evidence line's "4.08e13 already fits" corrected to the current target's figure.
- Added **ME25**: a new row for B7ACCT's v-slot accounting (RESULTS §113), citing it as the decision that superseded
  TS12/ME24's prior 4.08e13 choice.
- TS7/TS8/TS9: status changed from "IN FORCE" to "SUPERSEDED by TS13" (TS9 kept as "the standing-estimate *method*,
  still used at the current target" since its measurement inputs, not its digit count, are what's reused).
- TS12 (the TGTBENCH2 3.76e13 proposal): status **PROPOSED → SUPERSEDED by TS13** (the user picked the margin
  sibling, not the proposal itself).
- Added **TS13**: the target-size decision in force (3.71 × 10¹³), citing RESULTS §115 and results/S18TGT.md.
- Superseded-claims table: fixed S8 and S15's "Current:" columns, which pointed at 5.276e13 as still current.

### docs/code/06_EVALUATION.md
- Added a dated note (2026-10-06) directly under the title, explaining that most of Tier A/B and the §4.1
  recommendations are now done, that `comm_ofi` replaces the SHMEM-one-NIC-per-PE path described throughout, and
  that the target is now 3.71e13, not 5.276e13.
- Verdict section: rewrote all three bullets (transport readiness, the two blocking unknowns, the unit-of-injection
  recommendation) with "as written" / "2026-10-06: superseded/resolved/done" framing and current figures (373.44
  GB/node measured edge, 363.53 GB device layout at 3.71e13, STD17's 1669.98 s vs 3532.05 s).
- Component table: added a `comm_ofi` row next to the "as written" SHMEM row; rewrote the memory-system row's "Node
  471.9 of 480 GB" figure with the current target's numbers.
- §4.1 (recommendations table): added a status column, marked items 1, 2, 3, 4, 5, 7, 8, 10 **DONE**; item 6
  (documentation pass) **MOSTLY DONE** (step 3(c) already fixed; this pass covers the other docs; trap 17 / `MN_P24`
  print / mnaccept's module line not re-checked); item 11 **DONE as a mechanism** (correctness verified, no
  measurable gain on aac7's 1-NIC-per-APU hardware); item 13 **PARTLY DONE**; item 9 **OPEN** (flagged as the single
  largest unresolved risk, per EST17 §109); item 12 moot; item 14 untouched.

### ecalc/README.md
- Banner (the switch-table preamble): 4.08e13 → 3.71e13 as CURRENT, with 4.08e13 folded into the history chain
  (TGT17 → TGTBENCH2/s18-target).
- The Batch 3 "target-is" sentence: updated to show the full chain 5.276e13 → 4.08e13 (briefly) → 3.71e13 (current).

### ecalc/estimate.py (text only)
- `--target` flag's help text: 4.08e13-as-current → 3.71e13-as-current, with 4.08e13 added to the history list.
- The `target()` function's per-row label tuples: relabeled the `M.TARGET_DIGITS` row for TGTBENCH2/s18-target
  instead of TGT17; added a `4.08e13` history row; relabeled the `M.TARGET_BELOW` row for the current 3.6e13 value
  (it previously described B7ACCT's old 3.997/4.0e13 step, which belonged to the 4.08e13-target era).
- Verified `python3 -c "import ast; ast.parse(...)"` still parses the file after the edits.

## Contradictions found (not fixed — out of this task's scope, flagged for the user)

1. **The launch line's SHMEM/OFI pool sizing was not re-derived for the current target.** `docs/TARGET.md` §4's
   `COMM_SHMEM_POOL_MB=1536` / `SHMEM_SYMMETRIC_HEAP_SIZE=2048M` carry a comment explicitly citing "`plan pool` at
   5.276e13:576 with COMM_OFI=1" (Phase 17 OFIMEM) — i.e. these pool/heap sizes were computed for the *former*
   5.276 × 10¹³ headline, not for 4.08 × 10¹³ (TGT17) or the current 3.71 × 10¹³ target. Nothing in results/S18TGT.md
   or RESULTS §113–115 shows the pool being re-sized for 3.71e13 specifically. This is a sizing question, not a
   prose one, so it was left alone, but the user should know the launch line's pool size is stale relative to its
   own digit count.
2. **TGT17's 30 GB "B7 allowance" was an assumption that turned out wrong in a specific, traceable way.** TGT17
   (§111) assumed 30 GB headroom for an unmeasured general-map memory cost against the 480 GB *node* budget. B7ACCT
   (§113) later measured that cost directly (v-slots) and found it instead threatened the *device* edge (373 GB),
   not the node budget — a different bar than the one TGT17 was guarding. This is now explained correctly in the
   current docs (see docs/TARGET.md §1's CURRENT paragraph), but it's worth the user's attention as a pattern: two
   different generations of "add a safety margin" assumptions bound two different resources.
3. **docs/TARGET.md's trap 20 and §5's device-edge sweep (lines ~820-845)** still describe the 5.276e13 headline's
   CAP17 findings (405.42 GB device, etc.) as if they were the live picture. These are explicitly dated/historical
   (CAP17, 2026-10-06) and internally consistent, so left untouched — flagging only because a reader skimming that
   section without the dates could mistake it for current.
4. **`target_kit.sh`** (mentioned in the task as "coming on a separate branch, in progress") had in fact already
   merged into `origin/main` (commit 6db9cc9, "Merge s18-kit: target_kit.sh") by the time this branch was pushed.
   Per the task's explicit instruction this branch stays based on 63f50ca and mentions the kit only as "in progress"
   — not rebased onto the newer main, and not describing the kit's actual contents. The user should reconcile this
   branch with `s18-kit`'s main-line merge when both land.

## Files touched

`docs/code/00_OVERVIEW.md`, `docs/TARGET.md`, `docs/TARGET_TASKS.md`, `docs/code/05_DECISION_REGISTER.md`,
`docs/code/06_EVALUATION.md`, `ecalc/README.md`, `ecalc/estimate.py` — 7 files.

## RESUME

Complete. Branch `s18-docs` (from `origin/main` 63f50ca), to be pushed, not merged.
