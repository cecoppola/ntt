# DKM15: the division in two quotient halves with a half-length reciprocal (Phase 15 Batch 3, agent DKM)

Branch `p15-DKM` from `int15g` be2eec3. The item is SC's rank 2 (results/SC15.md §3.2), approved by the user on 2026-09-28.
The aac6 clone is `~/ntt-DKM15`. Times are Eastern. Every number is labelled **measured**, **modelled** or **assumed**.

The switch is `NEWTON_DKM=1`. It is off by default. With it off, the code paths are the ones on the base, unchanged.

## 1. Design note (stage 1, written 2026-09-28 17:50–18:40 EDT)

### 1.1 Today (`newton_db.c` on be2eec3)

The division computes X = floor(A / Q) with A = S·B^dl, S = P + Q, Q of n_q limbs, na = S.n + dl, and k = na − n_q + 1.
At the target, k ≈ dl ≈ n_q ≈ n = 2.83 × 10¹² limbs.

1. **The reciprocal.** `recip_db2` (size 1) or `recip_mn` (size > 1) computes mu ≈ B^(n_q+k)/Q to k limbs, with k + 1 limbs in all.
   It uses the anchored doubling chain k, ⌈k/2⌉, …. The last doubling (j = n/2 → n) forms two products: Q_t·r, a middle product
   under `NEWTON_RECIP_MID`, and r·d, cut below j under `NEWTON_RECIP_CUT`.
2. **The quotient estimate.** X₀ = ((S >> (n_q − 1 − dl))·mu) >> (k + 1). This is a high product, n × n, with the pieces below
   k + 1 skipped (`NEWTON_HIGHPROD`).
3. **The remainder window.** It forms X₀·Q mod B^w with w = n_q + 2. This is a low product, n × n, with the pieces above w skipped
   (`NEWTON_LOWPROD`). The window is A mod B^w = (S mod B^(w−dl))·B^dl.
4. **The corrections.** They run while R < 0 or R ≥ Q, with at most 64. The early writer starts on X₀ before the low product, through
   `newton_db_x_hook` / `newton_db_x_dev` at size 1 and `newton_mn_x_hook` (`MN_OUT_EARLY`) at size > 1. With
   `ECALC_CORR_PATCH=2`, the corrections are deferred (`newton_x_dx`), and the output stage patches the written tail.
   `ECALC_TEST_CORR=k` moves X₀ by −k before the hook.

The area in units of n², at piece granularity. Each is roughly the part of the a × b rectangle the cuts keep:
- The reciprocal: the last doubling is Q_t·r (the band between n/2 and n of an n × n/2 rectangle, 0.25) plus r·d (0.125). The chain
  adds a factor of 4/3, so ≈ 0.5 in all.
- The high product: 0.5.
- The low product: 0.5.
- The total is ≈ 1.5.

### 1.2 DKM (GMP's `mu_div` form, divide-and-conquer on the quotient)

Let s = min(⌊k/2⌋, dl), k₁ = k − s, and h = max(k₁, s + 1). With s = ⌊k/2⌋, h = ⌊k/2⌋ + 1. The steps:

- **The reciprocal to h limbs.** mu_h ≈ B^(n_q+h)/Q has h + 1 limbs. The chain stops one doubling earlier.
  - Size 1: `newton_db_recip(k_mu)` computes to h(k_mu), and the division truncates it as it does a longer kept mu today.
  - Size > 1: `recip_mn(h(k_mu))`.
  - Because h(·) is monotone, h(k_mu) ≥ h(k) for the actual k ≤ k_mu.
- **Step 1, the high half.** A′ = ⌊A / B^s⌋ = S·B^(dl−s), since s ≤ dl. This is the shifted form of today with dl₁ = dl − s, and
  step 1 is exactly today's division on (S, dl₁) with k₁ = k − s:
  - X_hi₀ = ((S >> (n_q − 1 − dl₁))·mu₁) >> (k₁ + 1), where mu₁ is mu_h's top k₁ + 1 limbs. This is a high product,
    (k₁ + 1) × (k₁ + 1), cut below k₁ + 1.
  - The window A′ mod B^w = (S mod B^(w−dl₁))·B^dl₁. The low product is X_hi₀·Q mod B^w, k₁ × n_q, cut above w.
  - The correction loop runs to 0 ≤ R₁ < Q and is applied to X_hi in place. There is no hook, no deferral and no test offset at
    this step. The writer never sees X_hi.
  - The result: X_hi = ⌊A′/Q⌋ and R₁ = A′ − X_hi·Q, both exact. S is freed here.
- **Step 2, the low half.** A₂ = R₁·B^s + (A mod B^s) = R₁·B^s, because A mod B^s = 0 when s ≤ dl. Also A₂ < Q·B^s, so
  ⌊A₂/Q⌋ < B^s. This is the shifted form again, with S₂ = R₁ and dl₂ = s:
  - k₂ = R₁.n + s − n_q + 1 ≤ s + 1, and mu₂ is mu_h's top k₂ + 1 limbs.
  - X_lo₀ = ((R₁ >> (n_q − 1 − s))·mu₂) >> (k₂ + 1). This is a high product, (s + 1) × (s + 2), cut below k₂ + 1.
  - If R₁.n + s < n_q, then A₂ < Q: X_lo = 0, and R = A₂, with no product.
- **Assembly.** X₀ = X_hi·B^s + X_lo₀, a shifted add. X_lo₀ can exceed B^s − 1 by a few units, and the add carries into X_hi's
  part. When X_lo₀ < B^s (almost always), the low product reads X_lo₀ as a view of X₀'s low s limbs, and X_lo₀ is freed.
  - `ECALC_TEST_CORR=k` is applied to X_lo₀ before the assembly. It needs X_lo₀ ≥ k for k > 0. X_lo₀ is ~s limbs, so this always
    holds at real sizes; the code stops with a message otherwise.
  - Then comes today's hook: the writer, and the residues at size 1.
- **The remainder.** The low product is X_lo₀·Q mod B^w, s × n_q, cut above w. The window is A₂ mod B^w = (R₁ mod B^(w−s))·B^s.
  - Today's correction loop and the output of dx are unchanged: deferred under `ECALC_CORR_PATCH`, in place otherwise, and R's
    residues come back as today.

**Exactness.** Each step is today's Barrett step followed by today's correction loop. The loop stops only when 0 ≤ R < Q, so each
step returns the exact floor and remainder whatever mu's error. mu's error sets only the number of corrections, which is ≤ 2–3 per
step as today, with a fatal stop at 64.
- A = A′·B^s = (X_hi·Q + R₁)·B^s = X_hi·Q·B^s + A₂.
- A₂ = X_lo·Q + R with 0 ≤ R < Q.
- So A = (X_hi·B^s + X_lo)·Q + R with 0 ≤ R < Q, and X = ⌊A/Q⌋ exactly.

The only new arithmetic is the assembly add, which is exact, and the choice of mu's length. A too-short kept mu falls back to today's
fresh reciprocal (`kept_ok`), which is correct but slow; the verbose line says so.

**The area**, in the same units:
- The reciprocal to n/2: 0.125.
- Step 1 high: 0.125. Step 1 low (n/2 × n mod B^n): 0.5 − 0.125 = 0.375.
- Step 2 high: 0.125. Step 2 low: 0.375.
- The total is ≈ 1.125, against 1.5 today (−25 %).
- The division alone rises 1.0 → 1.0, and all the gain is the reciprocal's last doubling. That matches SC's piece model:
  reciprocal 43.8 → 18.2 s, division 50.3 → 53.7 s (cache 2, modelled).

Two things could make the low products cheaper, and neither is in scope:
- **The wrap-around product** of GMP (X_hi·Q mod B^N − 1). The grid has no cyclic primitive.
- **A middle product for R₁'s top.** It is the same area, and it would give up exactness at step 1.

**Why not the host path.** `newton_db_divmod`, the host-A form, is not the production path: ecalc at size 1 uses
`newton_db_divmod_shifted` (`ovl3`). DKM is built into `newton_db_divmod_shifted` and `newton_mn_divmod` only. t_newton tests
`newton_db_divmod_shifted` directly (§1.6).

### 1.3 The corrections, the patch (`ECALC_CORR_PATCH=2`) and the early writer (`MN_OUT_EARLY`)

- **Step 1's corrections** are applied to X_hi before X₀ exists, so the writer, the patch and dx never see them. They are counted in
  a new field, `newton_st.dkm_corr`, and printed in the verbose line.
- **The final corrections** are today's. They are ± a few on X₀ = X_hi·B^s + X_lo₀, and they are deferred and patched, or applied in
  place (`db_add_small` / `mdb_add_val` on the whole X), as today. The patch zone (4096 digits) and the deferral contract of K15 §2.1
  are unchanged: `newton_x_defer` is read at the same point, and X is not changed after the hook.
- **The hook moves** from before the one low product to before step 2's low product. The writer then overlaps only that product,
  the window and the corrections (≈ 0.375 of the area) instead of today's 0.5. So the with-write gain is about half the no-write
  gain (SC: −16.2 of −31.9 s at cache 0, modelled).
- **A follow-up that would recover it (not built; outside my files).** After step 1's corrections, the top limbs of X are final:
  X = X_hi·B^s + X_lo with 0 ≤ X_lo < B^s exactly, so X's limbs at and above s are X_hi's. A writer hook after step 1 could therefore
  write X_hi's digits (the file's first ≈ half) during step 2's two products, with no patch risk to them. That needs changes in
  `mn_out.c` / `ecalc.c`: part files written in two ranges.
- **`ECALC_TEST_CORR=k`** keeps its meaning: X₀ − k before the hook, then k more final corrections. A second hook,
  `NEWTON_DKM_TEST_HI=k` (a test only, |k| ≤ 60), moves X_hi₀ by −k before step 1's loop. It forces step 1's corrections, which the
  final count must not show.

### 1.4 Memory (`mem_model.py`)

The per-device sets at the phase's peaks, n = n_Q share:
- **Today:**
  - The reciprocal's last doubling (`dm_layout` v2, DM_TIGHT) is P + Q + r (j + 1) + r2 (2j + 4) + t1 + a piece, with j = n/2.
  - The division's high product is Q + S + mu (k) + t (2k), ≈ 5n.
  - The low product is Q + X + Aw + xq, ≈ 4n.
- **DKM:**
  - The reciprocal's last doubling is at j = n/4, so r2 and t1 are halved.
  - Step 1's high product is Q + S + mu (n/2) + t (n), ≈ 3.5n.
  - Step 2's high product is Q + X_hi (n/2) + R₁ (n) + mu (n/2) + t (n), ≈ 4n. The window A₂'s is formed after it, then R₁ is freed.
  - The assembly is Q + X_hi + X_lo₀ + Aw₂ + X, ≈ 4n, briefly. Then X_hi and X_lo₀ are freed.
  - Step 2's low product is Q + X + Aw₂ + xq, ≈ 4n, as today.

At the target (modelled: `mem_model.mem_per_node(5.1e13/576, 576)`, the launch line), the arena is max(bs 201.8, dm 284.1,
tree 264.2) = 284.1 GB per node. The dm need binds, and its three parts per device are nearly equal:
- v2 (the reciprocal) 56.83 GB;
- v3 (the top bs levels beside the hole) 56.51 GB;
- div 55.45 GB;
- the hole (t / t1's block) 19.98 GB.

The saving comes only through a smaller hole: DKM's t is ≈ k limbs instead of 2k + 8, and t1 is halved. The hole is binsplit.c's
`dm_layout`, the layout the arena is carved to, which is not my file. So:
- **with the code as it is, DKM saves no node memory.** The blocks get smaller, but the arena is still sized for today's hole.
- **With a follow-up in `dm_layout`** (the hole at DKM's sizes when `NEWTON_DKM=1`), the dm need falls to max(v3′, div′), bounded
  below by the tree need. That is at most −19.9 GB per node at the target (modelled; the same bound as M6's).
  `mem_model.dm_layout(dkm=True)` prices it (stage 3), and `--check-c` stays exact because the default is unchanged.

At size 1, 10¹¹, the arena is bound by the bs regions (261.2 of 273.6 GB, modelled), so there is no memory change.

### 1.5 The model term (`mn_model.py`)

The term is `division_cost` under `MN_MODEL_DKM=1`, an environment knob like `MN_MODEL_CACHE_SLOTS`, so that P24's and MPB's
`Design` edits do not collide with it. It follows §1.2's exact lengths:
- `recip_cost` to h(k_mu);
- A′_h (k₁ + 1) × (k₁ + 1), with the low cut k₁ + 1;
- X_hi·Q, k₁ × n_q, with the high cut w;
- R₁'s top, (s + 1) × (s + 2), with the low cut s + 2;
- X_lo·Q, s × n_q, with the high cut w;
- the shifts: S → A′_h, t → X_hi, the window, R₁ → R₁_top, t → X_lo, the window, and X_hi → X for the assembly;
- the extra small ops: step 1's cmp / sub / corrections, and the assembly add.

The early writer's hidden part becomes step 2's low product plus the rest (`estimate.py`'s overlap). SC's `sc15_km.py` is the
reference: my term must reproduce its −22.1 / −31.9 s within the guard difference (h = ⌊k/2⌋ + 1 against SC's ⌈k/2⌉ + 2).

### 1.6 The test plan

1. **t_newton section 6** (`NEWTON_DEVICE=1`): `newton_db_divmod_shifted` with DKM off and on against GMP, on random
   (S, dl, Q) at n_q = 2¹⁰ … 2²⁰ (grids at the top under `DIST_LOGN_TEST`), in both bases. It checks X exactly and R's residues
   against GMP's R mod q.
   - k even and odd.
   - S near a multiple, so that R₁ is small and A₂ < Q gives X_lo = 0.
   - Q = B^k, and Q with all limbs B − 1.
   - Forced corrections: `ECALC_TEST_CORR` ±7 and `NEWTON_DKM_TEST_HI` ±7. The final count must be exactly k, and step 1's count
     must be at least |k|.
   - A kept mu that is too short (the fallback).
   - The existing sections must pass unchanged.
2. **Size 1**, both `NEWTON_DKM=0` and `=1`:
   - e9 in both bases against the references;
   - 4 × 10¹⁰ and 10¹¹ through `ecalc/digcmp.sh` (the packed default);
   - forced corrections at e9: `ECALC_CORR_PATCH=1/2 ECALC_TEST_CORR=±7`, `NEWTON_DKM_TEST_HI=7`.
3. **Timing at 10¹¹**: off / on interleaved, ≥ 3 each, both walls (`total` without the file, and the wall with it), the reference
   evicted, on one node and one job.
4. **Size > 1**: `mnaccept --only unit,e9,mn` with `NEWTON_DKM=1` (sizes 2–4), and `ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7` plus
   K15's long-chain cases (c108u / c108d at sizes 2, 4) with the switch on. The same with the switch off, as a sanity check.
5. **The models**: `mn_model` / `sc15`-style estimate at 5.1 × 10¹³ (cache 0 and 2) with `MN_MODEL_DKM=1`, and
   `mem_model --check-c` unchanged.

### 1.7 Verdict

**Safe.** The exactness rests on today's correction loop per step, which is unchanged. Each step is today's shifted division with
other arguments, and the one new operation, the assembly, is an exact add. The default path is untouched.

**Worth it.** At the target it saves −22…−32 s modelled without the write (−11…−16 s with it), the largest remaining item after
P24, at about a week. At size 1 it can be measured at 10¹¹: the reciprocal is ≈ a third of dm there.

**Caveats:**
- The memory gain needs a `dm_layout` follow-up (binsplit.c).
- The plan printer (`mn_plan.c`, P24's file) does not know the switch. Its piece counts under `NEWTON_DKM=1` would be today's, and
  its prime-bound check only gets safer, because DKM's products are smaller.

## 2. Stage 2: the size-1 form (the measurements follow in §4)

**Code** (`ecalc/newton_db.c`, `newton.h`, `tests/t_newton.c`; the README row):
- `newton_dkm_on()` / `newton_dkm_set()`: the switch.
- `newton_dkm_h(k) = ⌊k/2⌋ + 1`.
- `newton_db_recip` stops at h when `newton_db_Qd` is set, which is only ecalc's device-flow prewarm; t_newton's direct calls keep
  their k.
- `newton_db_divmod_shifted` dispatches to `divmod_shifted_dkm` when k ≥ 4 and dl ≥ 1. Its helpers:
  - `dkm_est_db`: today's estimate with mu's top as a view.
  - `dkm_window_db`: the window by a device shift. `db_set_shifted_low` copies limb by limb and is meant for a few limbs, but
    DKM's windows are ≈ n/2 limbs.
  - `dkm_corr_db`: today's correction loop.
- The assembly is `db_add_shifted`. X_lo₀ is read as a view of X's low s limbs whenever X_lo₀ < B^s.
- New fields and hooks: `newton_st.dkm_corr` (step 1's corrections) and `NEWTON_DKM_TEST_HI`.
- With the switch off, the only change in the default path is the one `if` of the dispatch.

**t_newton section 6** (run with `NEWTON_DEVICE=1`):
- 4 sizes (n_q = 1500, 2¹⁴ + 3, 2¹⁸ + 5, 2²⁰ + 1) × 7 shapes, off and on, against GMP: X exactly, and R's residues mod three primes.
- Forced corrections: `ECALC_TEST_CORR` ±7, where the final signed count must move by exactly k. `NEWTON_DKM_TEST_HI` ±7, where
  the final count must not move and step 1 must make them.

## 3. Stage 3: the multi-node form and the models

**Code.** `mn_divmod_dkm` (newton_db.c) is dispatched from `newton_mn_divmod` when dl ≥ 1 and k_mu ≥ 5.
- `recip_mn` runs to h(k_mu); a fresh reciprocal is taken if the actual h needs more.
- Step 1 is on S with dl − s. The window is `mdb_shift(S, −(dl − s), w)`. X_hi is in the basis of its length + 1, which leaves
  room for +1.
- Step 2 is on R₁ with s.
- The assembly is `mdb_shift` × 3 + `mdb_addsub`. X ends in the basis of its length, as today's X.
- `newton_mn_x_hook` is called there. X_lo's low product follows, then `newton_mn_pq_hook(1)` / `mfree(Q)` after it (they moved from
  after today's single low product), then today's corrections and deferral.

**Model** (`mn_model.py`: `division_cost_dkm` under `MN_MODEL_DKM=1`; `ovl_div`, the early writer's overlap, at size > 1). The
command is `python3 ../tests/dkm15_model.py` in `ecalc/`, and the output is `results/DKM15/model.txt`. All figures are modelled at
5.1 × 10¹³ on 576 nodes, with the fabric assumed, the write at 0.6 GB/s, and `ECALC_NP=auto`, on int15g:

| cache slots | today: no write / write | NEWTON_DKM=1 | gain | + an X_hi writer (not built) | gain |
|---|---|---|---|---|---|
| 2 | 330.7 / 360.2 s | 307.6 / 350.7 s | **−23.1 / −9.5 s** | 307.6 / 339.3 s | −23.1 / −20.9 s |
| 0 (what fits) | 406.8 / 426.0 s | 376.4 / 412.7 s | **−30.4 / −13.3 s** | 376.4 / 398.7 s | −30.4 / −27.3 s |

- The reciprocal goes 45.3 → 18.5 s and the division 53.3 → 57.0 s (cache 2). The pieces are recip 74 → 54 and div 40 → 42.
- DKM's four products are X_hi 9.4 s, X_hi·Q 17.1, X_lo 9.4 and X_lo·Q 17.1, with 7 / 14 / 7 / 14 pieces.
- These agree with SC's −22.1 / −31.9 s. SC's base was older, 324.3 s.
- The with-write gain is smaller than SC's estimate. The writer hides only under X_lo·Q and the rest, 17.1–23.9 s, against today's
  0.577 × the division. It is assumed fully overlapped.
- **A follow-up would recover it:** a writer hook after step 1 that writes X_hi's digits, which are final there, under step 2
  (`MN_MODEL_DKM_HI=1` prices it). It needs `mn_out.c` / `ecalc.c`: part files written in two ranges.

**Memory** (`mem_model.dm_layout(…, dkm=True)` / `mem_per_node(opts dkm=True)`; the default is unchanged, so `--check-c` is not
affected). **This corrects §1.4 and SC's ≤ −20 GB.**
- With a `dm_layout` that follows DKM (not built: binsplit.c), the node falls 455.4 → 450.3 GB at the target (**−5.1 GB**,
  modelled). At 10¹¹ it falls 410.4 → 405.0 GB.
- The hole halves (19.98 → 9.99 GB per device) and v3 falls 56.5 → 46.5. But the dm need is then bound by the division's
  low-product set, Q + X + Aw + X_lo·Q + X_lo ≈ 5.1 n per device, which is the same as today's.
- The −20 GB bound needs that set smaller as well: M6's chunked division window.
- **With the code as it is, there is no memory change.** The arena is carved to today's layout; DKM's blocks are smaller and fit
  in it.

## 4. Measurements (aac6)

### 4.1 Stage A: `NEWTON_DKM=1` correctness

Job 21766 ran on s24-26, 18:29–18:46 EDT, on commit 3fc51d6. The command was `ecalc/dkm15_batch.sh A 3fc51d6`, and the logs are in
aac6 `~/dkm15tmp/A/` and `~/ntt-DKM15/ecalc/results/mnaccept/21766/`. The first attempt, job 21760, was cancelled by the scheduler
after 50 s on s24-16 and ran nothing.

| test | result (measured) |
|---|---|
| `NEWTON_DEVICE=1 ./tests/t_newton 20` | **section 6: every case identical** (X exact, R's residues). This covers 4 sizes × 7 shapes, off / on, forced ±7 on both hooks. The final count moved by exactly k under `ECALC_TEST_CORR`. Under `NEWTON_DKM_TEST_HI` the final count stayed put and step 1 made the 7. The s = dl shape took the fresh reciprocal, as designed. **4 failures in section 5** (R4's "the high cut skipped nothing" at n_q 3·2²⁰, 2²²): that section needs `DIST_LOGN_TEST=20` (R415.md); stage B re-runs it so |
| e9 `NEWTON_DKM=1`, LIMB_BASE 10 / 2 | identical, VERIFY OK (`total` 10.81 / 22.78 s). **But at 10⁹ ecalc takes the host flow** (`newton_db_divmod`, below the device tier's 2³⁰), so DKM is not exercised at size 1 there. Stage B repeats these with `BS_MDEV_LOGL=24` |
| e9 forced (`ECALC_TEST_CORR` +7 / −7 / +7 with `ECALC_CORR_PATCH` 1 / 2 / 0; `NEWTON_DKM_TEST_HI=7`) | identical, VERIFY OK (host flow, as above) |
| c511 (511461828, +14) / c820 (820719000, −44), `ECALC_CORR_PATCH=1` | **VERIFY FAILED, digits differ.** Host flow, so not DKM: the §4.3 defect on main |
| **4 × 10¹⁰ `NEWTON_DKM=1`** | **identical** (digcmp, packed). DKM taken: `divmod(dev, DKM) 12.13 s: k 2222222224 = 1111111112 + 1111111112, h 1111111113 (kept); step 1 A mu 2.14, X_hi Q + corrections 3.48 (0), step 2 A mu + assembly 2.15, X_lo Q 3.64, corrections 0.45 (0)`. recip 3.94 s, dm 16.07 s, `total` 54.28 s |
| `NEWTON_DKM=1 ./mnaccept.sh 21766 --only mn` | **5 passed, 0 failed** (e8 sizes 2, 3, 4; e9 sizes 2, 4; DKM taken: `divmod(mn, DKM) … k 55555557 = 27777779 + 27777778`) |
| `NEWTON_DKM=1 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7 ./mnaccept.sh 21766 --only mn,recheck` | **7 passed, 0 failed** (at 10⁸ / 10⁹ the changed digits lie past d_out) |
| c108d (−39) at sizes 2 and 4, `ECALC_CORR_PATCH=0` (in place) | **identical**, all nodes VERIFY OK |
| `NEWTON_DKM_TEST_HI=−9` at sizes 2 and 4; +9 with `ECALC_TEST_CORR=−5` at size 3 | **identical**. Step 1 made the 9 corrections; the final count was 0 or 5 |
| c108u (+36) / c108d (−39) at sizes 2 and 4, `ECALC_CORR_PATCH=2` | **VERIFY FAILED on node 0, digits differ.** Same signature as c511: the patch on node 0's packed part file. See §4.3 |

**Reading.** In every run where the division took the DKM path (t_newton section 6, 4 × 10¹⁰, mn at sizes 2–4 including the forced
corrections), the digits are identical. The failures are all in `mn_out`'s tail patch on a packed file. They occur with
`ECALC_CORR_PATCH` ≥ 1 when the changed digits lie inside the file, and they appear in the host flow too, where DKM does not run.

### 4.2 Stage B: the gates with the switch off, the size-1 device flow at 10⁹, the patch controls

Job 21768 ran on s24-26, 18:46–19:19 EDT, on commit 5eb35dc. The command was `ecalc/dkm15_batch.sh B 5eb35dc`, and the logs are in
aac6 `~/dkm15tmp/B/` and `results/mnaccept/21768/`.

| test | result (measured) |
|---|---|
| `NEWTON_DEVICE=1 DIST_LOGN_TEST=20 ./tests/t_newton 20` | **VERIFY OK (1187 checks)**: sections 5 and 6 on grids |
| `./mnaccept.sh 21768 --only unit,e9` (switch off) | **10 passed, 0 failed**: t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid (2 procs); e9 in both bases identical |
| 4 × 10¹⁰, switch off | **identical**. recip 9.23 s, dm 20.90 s, `total` 57.50 s (A's on run: recip 3.94, dm 16.07, `total` 54.28; one run each, same node) |
| e9 through the device flow (`BS_MDEV_LOGL=24`), off / on | **identical**, VERIFY OK. The on run took DKM: `divmod(dev, DKM) … k 55555557 = 27777779 + 27777778, h 27777779 (kept)` |
| the same, forced: `ECALC_TEST_CORR` +7 (`ECALC_CORR_PATCH=1`), −7 (default), +7 (`=0`); `NEWTON_DKM_TEST_HI=7` | **identical**, VERIFY OK. The corrections were 0/7, 7/0, 0/7 and 0/0 |
| c511 / c820 through the device flow with DKM, `ECALC_CORR_PATCH=1` | VERIFY FAILED: the §4.3 defect |

### 4.3 A defect on main, independent of DKM: the tail patch on the packed output

Sent to the integrator on 2026-09-28. With `NEWTON_DKM` unset, on B2's code (job 21768):
- `./ecalc 511461828` with `ECALC_TEST_CORR=14`, under `ECALC_CORR_PATCH=1` and under the default (2): **VERIFY FAILED, digits
  differ**. With `ECALC_OUT_PACKED=0` the same run is **identical**.
- With `ECALC_TEST_CORR=1`, **a single correction**: **VERIFY FAILED**.
- `./ecalc 820719000` with −44: VERIFY FAILED.
- mnrun 2 at 108388422 (`POOL_LOG=27 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=36`): node 0 VERIFY FAILED. The ASCII form is identical.

The log says "patch: … holds 227320472 bytes, the patch ends at 511461830", then "patch … FAILED" and
"digits == X mod q FAILED". `mn_out_tail_fix` / `patch_bytes` (`mn_out.c`, not my file) write ASCII digits at ASCII offsets. A packed
part holds 8 bytes per 18-digit limb, so the offset lies past the end of the file.
- **Why the regression missed it:** its forced runs change only digits past d_out.
- **When it strikes:** any run whose division corrects X, with the changed digits inside the file (d_out near d), fails its VERIFY
  under the defaults.
- **What it means for DKM:** it does not bear on DKM. The same runs pass with `ECALC_CORR_PATCH=0`.

## RESUME

- **Committed:**
  - c00efbd: the note.
  - 0edd856: size 1 (`newton_db_divmod_shifted` → `divmod_shifted_dkm`, `newton_db_recip`'s length, `NEWTON_DKM_TEST_HI`, t_newton
    section 6).
  - 66d632c: size > 1 (`mn_divmod_dkm`) and the README row.
  - 3fc51d6: `ecalc/dkm15_batch.sh` (stages A, B, C).
  - 156ff25: the models (`MN_MODEL_DKM`, `mem_model.dm_layout(dkm)`, `tests/dkm15_model.py`, `results/DKM15/model.txt`).
- **aac6:** `~/ntt-DKM15` is at 3fc51d6. It builds clean, and the models do not need the node.
- **Stage A** (`dkm15_batch.sh A 3fc51d6`, the log in `~/dkm15tmp/A/batch.log`) was launched at 17:58 EDT. Job 21760 is pending
  behind the integrator's B2a/B2b and another user's job on s24-16.
- **Next:** read A's log. Then launch B (`… B <sha>`) and C, one at a time, each after the previous job has ended. Then fill §2–§4.
