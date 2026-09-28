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
