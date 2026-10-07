# S18DOCS2 — docs navigability pass (branch `s18-docs2` from `main` ccbbedd)

Docs-only pass: one index page, status banners on historical documents, stale "current" statements fixed in the
living docs, and `docs/AGENT_PROTOCOL.md` lessons from 2026-10-06/07. No code, RESULTS.md or results/*.md bodies
touched. No aac6/aac7 access used.

## 1. Inventory (as given; verified against the working tree)

| document | lines | last change | status assigned |
|---|---|---|---|
| ALGORITHM.md | 744 | 09-17 | HISTORICAL |
| CODE_REDUCTION.md | 132 | 09-22 | HISTORICAL |
| DECISIONS.md | 380 | 09-20 | HISTORICAL |
| DECISIONS2.md | 236 | 09-21 | HISTORICAL |
| DECISIONS3.md | 175 | 09-22 | HISTORICAL |
| DESIGN.md | 641 | 09-17 | HISTORICAL |
| PLAN.md | 2164 | 10-03 | HISTORICAL |
| RESULTS.md | 4568 | 10-07 | LIVING RECORD |
| TASKS.md | 368 | 09-25 | HISTORICAL |
| docs/AGENT_PROTOCOL.md | 56→69 | 09-? / this pass | CURRENT |
| docs/APUMULT_STUDY.md | 217 | 09-24 (banner 09-26) | HISTORICAL |
| docs/TARGET.md | 857 | current | LIVING RECORD |
| docs/TARGET_TASKS.md | 124 | current | LIVING RECORD |
| docs/TARGET_WISHLIST.md | 351→~360 | current (this pass) | LIVING RECORD |
| docs/code/00–07 | — | 2026-10-05/06 | CURRENT (00, 05, 06 touched this pass) |
| ecalc/README.md | — | 2026-10-05 header | CURRENT |
| results/*.md | ≈120 | various | evidence, not narrative |

## 2. What was added

- **`docs/README.md`** (new, 63 lines): one row per document above (what it is / status / when to read it), a
  "start here" order (00_OVERVIEW → TARGET.md §4 → 06_EVALUATION → RESULTS latest), and a pointer to `results/` by
  prefix rather than all ≈120 rows.
- **Status banners** (3–5 lines, below the title, body untouched) on: `ALGORITHM.md`, `DESIGN.md`, `DECISIONS.md`,
  `DECISIONS2.md`, `DECISIONS3.md`, `CODE_REDUCTION.md`, `TASKS.md`, `PLAN.md`, `docs/APUMULT_STUDY.md`. Each says
  the period covered, what supersedes it, and that the body stays as history.
- **`docs/AGENT_PROTOCOL.md`**: an 8-line "Lessons of 2026-10-06/07" section before "Gates for a code change":
  `tools/rundriver.sh` for aac7 drivers, never launch on aac7 after returning, never stop to "wait", one network
  program per node (CXI CMDQ collision, RESULTS §118), uneven group steps are fine, no digit writes to the aac7
  NFS home on 10-node runs (RESULTS §116), export `SLURM_JOB_ID` before `mnrun.sh`.

## 3. Stale statements fixed (file:line before the edit)

| file:line | said | now |
|---|---|---|
| `docs/code/00_OVERVIEW.md:136-137` | `ECALC_VSLOT_BUDGET` defaults to 0 | split into two bullets; the 2026-10-07 decision (default 1, RESULTS §116) added as its own line |
| `docs/code/00_OVERVIEW.md:230` | `ecalc/README.md`'s header "as of 2026-09-28" is stale (S17) | README's header is now "2026-10-05"; S17's complaint is resolved — note updated to point at the switch table itself for anything newer |
| `docs/code/05_DECISION_REGISTER.md:108` (ME25) | `ECALC_VSLOT_BUDGET` default 0, "IN FORCE (switch off)" | marked **SUPERSEDED by ME26**; new row ME26 added (default flipped to 1, RESULTS §116, commit ccbbedd) — ME25's body otherwise untouched |
| `docs/code/06_EVALUATION.md` §4.1 item 9 | "OPEN: comm_ofi's own 4→10-node data is flat... still the largest unresolved risk" | marked **DONE at aac7 scale** (RESULTS §117 sweep); A4 at 576-node multi-group scale is now named as the remaining risk instead |
| `docs/TARGET_WISHLIST.md` (TGTBENCH2 note) | "Proposed target size: 3.76 × 10¹³" | marked superseded, pointer to the actual 2026-10-06 decision (3.71 × 10¹³) |
| `docs/TARGET_WISHLIST.md` (S18KIT note) | "Not yet run on the target itself" (true, but left A3 looking untouched since S18KIT) | added the 2026-10-07 S18KITFIX note: kit bugs fixed (ef22f22), stage a3 passed on aac7 2 nodes (RESULTS §116) — still not the target itself, A3/A4 at target scale stay OPEN |
| `docs/TARGET_WISHLIST.md` (standing-estimate line) | "Today's standing estimate... 5.276 × 10¹³... 290.2/303.9 s" presented with no pointer forward | marked superseded with a pointer to `00_OVERVIEW.md`'s current standing estimate (3.71 × 10¹³, 221.9/225.1 s) |

## 4. Contradictory or unresolved items (not guessed, left as-is)

- **`docs/code/02_PIPELINE_MEMORY.md`, `03_DISTRIBUTION_COMM.md`, `04_OUTPUT_LAUNCH_MODELS.md`, `07_COMM_OFI.md`**
  still carry the old 5.276 × 10¹³ (and, in one 04 table row, 5.167 × 10¹³) target figure in illustrative model
  tables and sensitivity rows (e.g. `04_OUTPUT_LAUNCH_MODELS.md:342,408,473-474,484,499,583,610,613`;
  `03_DISTRIBUTION_COMM.md:525`; `02_PIPELINE_MEMORY.md:73,437`; `07_COMM_OFI.md:60`). `00_OVERVIEW.md` already
  frames all of these as "the former 5.276 × 10¹³ headline" history and is the entry point, but the rows inside
  01–04/07 themselves have no inline "superseded" marker. Left unedited: these are detailed, file:line-cited
  measurement/model tables (02–04, 07 are themselves cited by 00 as "each written from a full reading of their
  files" — rewriting their body figures risks silently changing a cited number without re-deriving it, which is
  outside a docs-only, no-code-run pass). Flagging for the owner rather than guessing at a rewrite.
- No other outright contradictions found between `RESULTS.md` §105–118, `docs/code/00_OVERVIEW.md`'s "Defaults
  chosen for you", and the current code (`ecalc/binsplit.c`, `ecalc/README.md`) — `ECALC_VSLOT_BUDGET`, `COMM_OFI`
  default, and the 3.71 × 10¹³ target all agree after the fixes above.

## RESUME

Done: tasks 1–5 of the brief. If resuming: everything above is committed on `s18-docs2`; nothing else in the
Markdown set was found stale against the "current truths" list in the brief beyond what §4 lists. A possible
follow-up (not done here, out of scope for a docs-only pass): add "superseded, see 00_OVERVIEW §1" footnotes to the
specific stale-figure lines in 02/03/04/07 named in §4 above, if the owner wants them touched.
