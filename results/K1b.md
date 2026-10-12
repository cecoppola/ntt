# K1b: raw-once gather (S39 candidate A, O7b lever 3) -- IMPLEMENTED behind DIST_GATHER_RAW (off), probe + gates + ABBA ARMED on aac6

Branch k1b (from origin/main a448d73). Labels: **m** measured, **mod** modelled, **as** assumed. Times Eastern.

## 1. Question
O7b (results/O7b.md, on branch o7b): k_gather is the largest kernel at 6.441e10 on one node (6.34 s APU0, 174 calls x 36.5 ms per APU, 15 % of GPU time, m), plus k_bload 1.2 s. S39 candidate A: gather each operand once without the canon, then canonicalise per prime with a local elementwise pass; O7b modelled 2.5-3.6 s.

Prior evidence that bears on it: S40 (aac7) ran the "local-only source" micro-test that S39 asked for: remote 36.1 ms vs local 35.9 ms with the select kernel (m). So the gather is **not** bound by the remote pull, and S40 concluded "do not build A as specified". That conclusion only removes S39's reason (fewer remote pulls); it does not decide the design. Raw-once saves whenever the gather itself (whatever bounds it: the strided store, the 64-bit `/` and `%` index arithmetic, the remote read) costs much more than a coalesced canon pass, because the gather then runs once per operand instead of once per prime. S50's tile kernel (coalesced store) bought only 5 ms of 35 ms (m), so the store alone is not the bound either. The right micro-test is therefore the design's own two pieces: raw gather vs gather + canon, and the canon pass, at the product shape.

## 2. Micro-test (tests/t_gather_raw_probe.c, `make tests/t_gather_raw_probe`)
Copies of k_gather<1> on APU 0 with the source striped over the 4 APUs (the product's case), rows 8192 x C 65536 (2^29 points, the 6.441e10 shape, as S40) and rows 1024 x C 524288:
V0 gather + canon (the product), V1 gather raw, V2 canon pass plane -> plane, V3 canon pass in place, V4 gather + canon with a power-of-two index (shift / mask; information only, a possible cheaper lever), V5 raw gather from a local-only source (S40's check repeated on aac6). V2, V3, V4 must equal V0 bit for bit (checked), V5 must equal V1.
Model printed: per operand at np primes, now np x V0, raw-once V1 + (np-1) V2 + V3; RATIO4 = their ratio at np = 4.
Run-level model used by the driver's stop rule: saving = 174 calls x 36.5 ms x (1 - RATIO4) (calls and ms: O7b, m; assumes every gather call is covered, i.e. DIST_GATHER_RAW=2 and np = 4 throughout, as). Continue to the gates and the ABBA only if that is >= 1.5 s.

## 3. Implementation (rns_dist.c, dist_core only: the single-node C-form product; the multi-node tier is untouched, its k_gather_mn24_np already reads once for all planes)
- `k_gather_raw<QS>` = k_gather without `ec_canon64`; `k_canon_pass` = elementwise `ec_canon64` plane -> plane; `gather_raw_all()` gathers raw into the last plane and canonicalises all np planes from it (the last in place, last).
- `DIST_GATHER_RAW=1`: A (miss) through its own xa[np-1]; B (miss) through its cache plane cb when it has one (and cb != ca). **No extra memory.** B without a cache plane: per prime as before.
- `DIST_GATHER_RAW=2`: additionally B without a cache plane through one extra plane of q limbs (4.29 GB per APU at 2^29 points, mod) taken from the block pool's **free extents only** (`db_pool_fit_count >= 1`, no growth, no hipMalloc) and only if `DIST_GATHER_RAW_MARGIN_GB` (default 2) stays free afterwards; otherwise B per prime (one note printed). Freed after the prime loop.
- Digits: canon(raw limb) is exactly the old gather's value, zero beyond the operand stays zero; identical by construction. Gates below check it.
- Memory and the margin rule: the extra plane is drawn from memory that is already mapped (the arena / pool sized at init), so the device total cannot grow through it; the rule "rank-0 margin >= 10 GB at the target size" (L1: 12.9 GB at 3.82e13 with VSLOT_RELEASE, mod) concerns that total and is unaffected by construction. What can change is the pool's peak live (up to +4.29 GB per APU) and, if another concurrent allocation then misses the free extents, pool growth by hipMalloc inside a phase: the driver reports device in use, pool peak live and pool hipMalloc per arm. At the target the products that use dist_core are the per-node ones below the grid tier (as); level 1 needs no memory at all.
- k_bload (B form, 1.2 s per APU) is not changed (O7b: no k_gather / k_bload overlap in one product; a shared raw staging for it is follow-up work).
- Switch rows: ecalc/README.md (one row), registry (switches_manual.tsv, regenerated switches.tsv / switches_gen.h; the regeneration also refreshed 4 stale rows from main: BI_MUL1_FAST, NEWTON_RECIP_CUT, NEWTON_RECIP_MID removed as having no code site, COMM_HS_TS added, COMM_LAYER_VSLOT_RELEASE scope; `--check` 0 problems).

## 4. Driver and gates (tools/k1b_driver.sh, armed on aac6)
- Own hold: `sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J k1b` (no -w), queued behind our other jobs; give-up 12:00 Eastern (cancels only its own id).
- Step 1 probe (both shapes) -> model -> stop if < 1.5 s ("SUCCESS: refuted ...").
- Step 2 `tools/gate.sh --build --nodes 1 --hold J --env "RNS_STRATEGY=C DIST_GATHER_RAW=2"`, then without --build for =1 and =0 (1e9 digits against the 1e9 reference; RNS_STRATEGY=C because at 1e9 every product is the B form by default (O7b) and the gate would not reach the changed code otherwise).
- Step 3 ABBA at 6.441e10, A = DIST_GATHER_RAW=0, B = 2, no digit output, archive/drivers/s50/s50_abba6.sh (rounds ABBA / BAAB, paired t on round deltas, stop p<0.05 after >= 3 rounds, cap 5, or the hold's end); every run must print VERIFY OK.
- Step 4 memory per arm from the runs' `mem_report` lines; summary, marker, scancel of its own job, `squeue -s` shows no step.
- Dry run with stub sbatch / srun / squeue / gate (both the "confirmed" and the "refuted" path): summary, marker, scancel of the own id only: OK.

## 5. Results
(pending; filled on collection)

## RESUME
- State: code + probe + driver on branch k1b (pushed). aac6 clone `~/ntt-k1b` (from a full bundle; the shared clone `~/ntt` is on the pre-rewrite history), built (`module load rocm; make -s -j16`, probe built). Driver armed 2026-10-12 02:32 Eastern (`setsid nohup`, GIVEUP_EPOCH 12:00 Eastern), hold job 22989 pending third in the PPAC queue behind two of our jobs; expected start ~03:05-03:50, DONE ~03:50-04:35 Eastern (as). Output `~/k1b/`.
- Collect: `ssh aac6 'cat ~/k1b/DONE; cat ~/k1b/summary.txt'` (one bounded command). Marker text: `SUCCESS: refuted by the probe ...` (write up: section 5 + verdict), `SUCCESS: probe ... gates PASS; DONE rounds=...` (copy the ABBA line, memory lines and probe numbers into section 5), or `FAILED: ...` (see summary.txt; probe.log, gate_*/, abba/log/).
- After collection: remove `~/ntt-k1b` and `~/k1b.bundle` on aac6 when the branch is decided; local worktree `../ntt-wt-k1b`.
