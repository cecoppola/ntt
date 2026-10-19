# PRIMES_STRATEGY — prime count and strategy

Scope: prime count and strategy: four primes only where needed, 24-digit points, auto switch-over, strategy boundary and plane cap, roots above 2^33. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| P2415 | four primes at 24 digits per transform point in the mn tier (Phase 15 Batch 3, agent P24) |
| NP15 | NP (Phase 15 Batch 2, PLAN §37 row 1) — four primes only where needed (`ECALC_NP=auto`) |
| MPB15 | MPB (Phase 15 Batch 3): the `ECALC_NP=auto` switch-over on min(pa, pb) (`ECALC_NP_AUTO_MIN=1`) |
| P15 | P (Phase 15, PLAN §36, Batch 1 from L8's findings) — roots above 2³³, the prime bound at the 576 target |
| MEAS-P24 | four-prime product form (P24) for all multi-node products against the default, 2 nodes |
| S13 | Phase 13a, agent S — E0 (the A/B/C strategy boundary) and E2 (plane cap against grid cost) |
| B13b | Phase 13b, agent B: strategy B in the library, and B4 (PLAN §31, axis S) |

## P2415: four primes at 24 digits per transform point in the mn tier (Phase 15 Batch 3, agent P24)

(former file: P2415.md)

Branch `p15-P24` from `int15g` be2eec3. The item is SC's §3.1 (results/TARGET.md §SC15), which the user approved on 2026-09-28.
Every number is labelled **measured**, **modelled** or **assumed**. All times are Eastern.

Status (2026-09-28 20:15 EDT): **stages 1–4 are done.**
- Every test passed on one node (s24-30). The digits are identical under `MN_P24=1` and `2`, at `ECALC_NP=4` and `auto`, at 2, 3
  and 4 node-processes.
- The switch-off gates pass.
- 10¹⁰ at size 2 is **158.1 → 97.0 s (measured, −38 %)**.
- The C plan at 5.1 × 10¹³ is 222 → 158 critical-path pieces.
- The modelled target gain at cache 0 is −85 to −110 s.

The summary is in §5.

#### 1. Design note (stage 1, 2026-09-28 18:00 EDT)

##### 1.1 The idea and the bound

Today one transform point of an mn product carries one base-10¹⁸ limb. With four primes (p₀p₁p₂p₃ = 2^206.55), a point can
carry a 24-digit value instead: 4 limbs (72 digits) become 3 points. Point x of an operand view holds the view's digits
[24x, 24x + 24):
- its low part is limb L(x) = ⌊4x/3⌋ from digit s(x) = 6·(x mod 3), and
- its high part is the low 6 + s digits of limb L(x) + 1.

A view of n limbs has P(n) = ⌈3n/4⌉ points. The plane of a piece of pa + pb limbs therefore holds P(pa) + P(pb) points, 3/4 of today's.

**The coefficient bound.** A coefficient of the cyclic product is at most min(Pa, Pb)·(10²⁴ − 1)² (Pa, Pb = the pieces' points; no
wrap, since Pa + Pb ≤ the plane).

| check | largest min(Pa, Pb) allowed | at the 2⁴⁰ cap (min ≤ 2³⁹) | what needs it |
|---|---|---|---|
| Garner (four primes): min·(10²⁴−1)² < p₀p₁p₂p₃ | 1.51 × 10¹⁴ = 2^47.1 | 2^198.45 < 2^206.55: ×275 (×137 on pa + pb = 2⁴⁰, the task's form) | the CRT's exactness |
| the CRT's four-limb output: c·10^s < 10⁷² for s ≤ 12, i.e. c < 10⁶⁰ | 10¹² = 2^39.86 | 2^238.3 < 2^239.2: ×1.82 | the spill block of four limbs (below) |

The binding check is the second one, not the primes. The code therefore caps a P24 plane at 2⁴⁰ points (P24_CAP_LOG). The target's
576-node cap is exactly 2⁴⁰; groups of 1024 or more nodes, whose cap is 2⁴¹ or larger, stay at 2⁴⁰ for P24 pieces. As a second
guard, `mn_core` refuses a P24 piece with min(Pa, Pb) > 10¹². As a third, the CRT kernel raises a device flag if a fifth limb
would be nonzero (it checks for the top digit d₃ ≥ 10^(18−s), which cannot happen under the first two guards); the host stops on it.

**Digits byte-identical by construction.** It is the same exact product, with no rounding anywhere.

##### 1.2 Where the regrouping happens (rns_dist.c `mn_core`; new files `p24.h`, `p24_mn.h`)

Rank ρ holds rows [a, a + r) of the R × C plane, with a = part0(R, nr, ρ) and r = partn. So in column j it holds the points
[x₀, x₁) = [Rj + a, Rj + a + r). The limbs of those points form a contiguous **run**:
- **in-run** (operand side): [L(x₀), L(x₁) + 1). The last point's high part is at most limb L(x₁).
- **out-run** (result side): [L(x₀), L(x₁)). These tile the limb line exactly, rank after rank and column after column, because L
  is monotone and the points tile.

A rank's sequence is its runs in column order, as today's sequence is its rows in column order. S(j) = Σ_{j'<j} len(j') has a
closed form: L(x) = x + ⌊x/3⌋, and Σ_{j'<j} ⌊(Rj' + c)/3⌋ is periodic in j with period 3, since R mod 3 ∈ {1, 2}. That gives
`p24_seq_start(l)` (the first sequence index with limb ≥ l) and `p24_seq_limb(t)` (its inverse) in O(1) on host and device.
Every piece of today's exchange machinery keeps its shape; only the index maps change:

| today (rns_dist.c) | P24 (the functions touched or added) |
|---|---|
| `seg_tables` / `redistribute`: segments via `seq_start` (point = limb) | `seg_tables` and `redistribute` take a map flag: segments via `p24_seq_start` on in-runs; `k_pack_mn24` packs limb `p24_seq_limb(t)` |
| `k_gather_mn`: rb[t] mod p into the row layout | `k_gather_mn24`: point (il, j): its two limbs from the in-run, v = ⌊lo / 10^s⌋ + (hi mod 10^(6+s))·10^(18−s), reduced mod p as (lo part) + (hi part)·(10^(18−s) mod p), into the same row layout |
| `k_crt_batch` (rns_mul.c): stripe = the rank's rows of column j, Garner → 4 base-10¹⁸ digits at the point's limb, carry, a 4-limb spill at the next rank's first row | `k_crt24` (new, `p24_mn.h`): Garner → 4 base-10¹⁸ digits d of c → shifted by s(x) digits into e₀..e₃ at limbs L(x)..L(x)+3 (at most 3 points meet a limb: sums < 3·10¹⁸ + carry < 2⁶⁴), the carry chain over the stripe's out-run, a **4-limb spill at L(x₁)**, the next out-run's first limb. The limb after the last point's e₃ is L(x₁ − 1) + 4 ≤ L(x₁) + 3, so four limbs hold it (with the carry: the bound in 1.1) |
| the result exchange: `hsO`/`hsI` via `seq_start`, `k_scatter_mn` | the same via `p24_seq_start` on out-runs; `k_scatter_mn24` writes limb `p24_seq_limb(t)` |
| `spill_cols` (the columns whose block [Rj + a', +4) meets a window) | `p24_spill_cols`: blocks at [L(Rj + a'), +4) |
| `db_share_add_spills_x` → dbig.c `sparse_get`: limb m → (column, next rank's row, offset < 4) | dbig.c (a minimal, commented edit outside my files): `struct sparse` gets `p24`; `sparse_get` decodes limb m through the point x = the largest with L(x) ≤ m. That gives (j, the owning row's rank q), and the offset is m − L(Rj + part0(q)). The spill blocks are ≥ 42 limbs apart (r ≥ 32 rows), so a limb meets at most one. New entry `db_share_add_spills_x24` |
| the plane: `mn_shape(nc)` on limbs | `mn_shape(P(pa) + P(pb))` on points; the pools (q per prime, xb) are unchanged in size |
| `xb` (pool 1, q + 16) = the CRT output (a point per limb) | the out-runs hold ≤ 4/3 q + C limbs: written into pool 1's `xb | sl` (3q + 16 words; the slabs are idle after the transposes) |
| the added operand X rides in the CRT (one plane) | a P24 product adds X afterwards with `mdb_add_shifted` (as the grid path does) |
| the transform cache (A1 over shares) | not used by a P24 piece (the likely target baseline is cache 0 anyway: SC §0). In `mn_grid` the slots are left untouched, and a pin request is cleared |

The windows, the rounds (`MN_T_CHUNK_MB`, `mn_round_add`), the node carry scans (`share_carry_fix`), the cuts and the direct path are
all in piece limb coordinates, so they are unchanged. The per-chunk carry bound is also unchanged: the out-runs are normalised
(< B) and exactly one spill block meets a limb, so a window limb is < 2B.

**Pieces need no alignment.** Each view regroups from its own limb 0. Piece (i, j) is ∑ c_x 10^(24x) · B^(oa + ob), with oa and ob in
limbs as today, so there is no multiple-of-4 constraint on the grid.

**The operands' sequences are 4/3 longer per plane at the cap** (limbs, not points). `mn_core`'s P24 path frees A's received sequence right
after A's gathers, before B's redistribution. The operand-side peak is then max(A, B) instead of A + B, which is below today's
(modelled at the target cap per APU: 2 × 2q/3 = 1.33 q against 1.5 q). `rns_mul_dist_mn_scratch` / `_stage` get the P24 branch
(same formula structure, in-run lengths). mem_model.py (not my file) is not changed: `mem_model --check-c` stays exact at the
defaults. With MN_P24 on, the C layout sizes the arena itself.

##### 1.3 The switch and `ECALC_NP=auto`

`MN_P24` (default 0: the code is unchanged, byte for byte and path for path):
- **1**: a product runs P24 when its 18-digit grid's largest piece would run four primes (`ec_np_for(pa + pb) == 4`: every mn
  product under `ECALC_NP=4`; under `auto`, those over the three-prime bound). Three-prime products stay at 18 digits (the task's
  rule). A three-prime product at 24 digits is impossible: 2⁰·(10²⁴)² > p₀p₁p₂.
- **2**: every mn product at P24 and four primes. This is SC's model (§3.1 "every mn piece at four primes"). It needs pool 0 at
  four planes, which is what `ECALC_NP=4` makes, and `auto` at the target makes (ec_np_planes); at small `auto` sizes use
  `ECALC_NP_AUTO_TERMS`.

The decision is one function, `mn_p24_of(na, nb, g)`. It is called by `mn_grid`, `rns_mul_dist_mn_shape`, `rns_dist_mn_plan`,
`rns_mul_dist_mn_scratch` and `_stage`, so the run, the tests' references, the plan and the sizing agree. Its prime-count test is
the same call `mn_core` makes today (`ec_np_for(pa + pb)`). If agent MPB changes that rule (min(pa, pb)), the one line in
`mn_p24_of` follows it; the two changes are otherwise separable. The P24 grid is `p24_split`: split_grid_cap's rule
(i, j ≤ 32, the fewest plane points, then the fewest pieces), with the constraint on P(pa) + P(pb) ≤ min(cap, 2⁴⁰) points.

Functions touched in rns_dist.c: `mn_core` (the P24 branches, named `P24:` in comments), `mn_grid`, `seg_tables`, `redistribute`,
`mn_round_add` (the spill call), `rns_mul_dist_mn_shape`, `rns_dist_mn_plan`, `rns_mul_dist_mn_scratch`, `rns_mul_dist_mn_stage`;
new: `mn_p24_on`, `mn_p24_of`, `p24_split`, `p24_spill_cols`. `dist_core` (the one-node tier) is not touched: SC found three
primes at 18 digits as cheap there, and MPB owns its prime-count choice.

##### 1.4 The model (mn_model.py, the P24 term; `tests/p24_model.py`)

- `mn_model.P24` (env `MN_P24`, the same values) and `p24_of` mirror the code's decision.
- `_product_cost_p24` prices a P24 product:
  - the grid is `p24_split` on points at min(cap, 2⁴⁰);
  - every piece is at four primes, at its plane's points;
  - the redistributions and the result exchange move limbs;
  - no cache;
  - X is added by the shifted add.
- `MN_MODEL_P24_F` is the local passes' surcharge, **assumed** 1.0 until measured (SC's range 1.05–1.20).

At 5.1 × 10¹³ on 576 nodes (**modelled**; the fabric **assumed** 100 GB/s per APU, 2 µs; the write at 0.6 GB/s; node 0's pieces
= tree + recip + div):

| ECALC_NP, cache slots | today (MN_P24=0) | MN_P24=1 | MN_P24=1, F 1.10 | MN_P24=2 | MN_P24=2, F 1.10 |
|---|---|---|---|---|---|
| 4, 0 | 430.2 / 449.3 s, 222 pieces | 317.2 / 352.3 s, 158 | 331.7 / 365.3 | (= 1) | |
| 4, 2 | 349.4 / 378.8 s, 222 | 317.2 / 352.3 s, 158 | 331.7 / 365.3 | (= 1) | |
| auto, 0 (SC's likely baseline) | 406.8 / 426.0 s, 222 | 319.3 / 354.4 s, 166 | 329.5 / 363.1 | **311.0 / 346.2 s, 158** | 325.5 / 359.1 |
| auto, 2 | 330.7 / 360.2 s, 222 | 303.3 / 338.5 s, 166 | 313.6 / 347.2 | 311.0 / 346.2 s, 158 | 325.5 / 359.1 |

At 5.55 × 10¹³ (the node 478.7 GB, **modelled**):

| ECALC_NP, cache slots | today | MN_P24=1 | MN_P24=2 |
|---|---|---|---|
| auto, 0 | 493.2 / 511.6 s, 259 pieces | 371.6 / 406.9 s, 194 | **343.7 / 379.0 s, 180** |
| auto, 2 | 394.2 / 424.4 s, 259 | 349.5 / 384.8 s, 194 | 343.7 / 379.0 s, 180 |
| 4, 0 | 523.4 / 541.8 s, 259 | 351.0 / 386.3 s, 180 | (= 1) |

**Verdict: safe and worth it** (modelled).
- At cache 0 the gain is −88 to −113 s without the write at 5.1 × 10¹³. SC had −94.5.
- 5.55 × 10¹³ under MN_P24=2 (343.7 s) runs faster than today's 5.1 × 10¹³ at cache 0 (406.8 s).
- MN_P24=1 under auto keeps the three-prime tree levels at 18 digits. That is worse than 2 at cache 0, and better at cache 2,
  because 1 keeps the cache for those levels.
- The C plan (stage 4) will replace these piece counts with the code's own.

##### 1.5 Test plan and stages

1. **Stage 2, `tests/t_p24.c`** (host part: gcc on the login node; device part: hipcc on a node). Against slow references:
   - the maps (L, the point floor, S(j), seq_start/seq_limb both ways, the spill decode, `p24_spill_cols`) against brute force over
     R ∈ {2¹⁰, 2¹¹}, nr ∈ {8, 12, 24}, every rank;
   - the regroup (limbs → points → residues mod each prime) against a big-integer reference;
   - the ungroup and the 24-digit CRT carry: random and boundary coefficients (all 9s, 10²⁴ − 1 points, carries across groups
     and stripes, a coefficient at the 10⁶⁰ bound) through a host model of `k_crt24` and the kernel itself, summed into limbs
     against an exact reference.
2. **Stage 3**, one node, 2–4 node-processes (`mnrun.sh`), lowered caps (`t_mn_grid` uses `DIST_LOGN_TEST=24`; POOL_LOG 27–29):
   - `MN_P24=1 ECALC_NP=4` and `MN_P24=1/2 ECALC_NP=auto ECALC_NP_AUTO_TERMS=<low>`: `t_mn_grid` at 2, 3 (the general map) and 4
     processes;
   - `mnaccept.sh --only unit,e9,mn`: identical digits at 10⁸ (sizes 2–4) and 10⁹ (sizes 2, 4);
   - with the switch off, the gates: `t_newton`, `t_mul`, e9 both bases, and 4 × 10¹⁰ identical.
3. **Stage 4**: `MN_PLAN_ONLY` with `MN_P24` (the plan check with the P24 bound), the piece counts at 5.1 and 5.55 × 10¹³, and
   mn_model against the C plan.

##### 1.6 Risks

- The kernels are new code in the hottest path. Each map has a brute-force test (stage 2) before the end-to-end tests.
- The CRT's serial carry walks ~4/3 limbs per chunk. The throughput cost is unknown; SC's surcharge is 1.05–1.20 (assumed). A
  size-2 timing pair prices it if time allows.
- The general map (g not a power of two) and the chunked rounds use the same maps. Both are covered by t_mn_grid at 3 processes
  and by `MN_T_CHUNK_MB` small in stage 3.

#### 2. Stage 2: the unit test (`ecalc/tests/t_p24.c`)

New files: `ecalc/p24.h` (the maps, the regroup's parts and residue, the CRT's digit shift) and `ecalc/p24_crt.c` (`k_crt24`, with
a copy of rns_mul.c's device Garner, which is static there, and the bound flag). The Makefile gets `p24_crt.o` in OBJS and
`tests/t_p24` in TESTS; this is a minimal edit outside my files.

The test runs against slow references:
- **the maps**, against brute force: R ∈ {16, 24, 1024, 2048}; C = 7, 64 and 1024 (sampled); nr ∈ {4, 8, 12, 24}, including the
  unequal parts; every rank. It checks the prefix S(j), `p24_seq_limb` for every index, `p24_seq_start` for every limb, that the
  out-runs tile the limbs, `p24_spill_at` for every limb, and `p24_spill_cols` on 400 windows (random and at the blocks' edges);
- **the regroup**: n ∈ {1–9, 100, 1001, 4096} limbs × {random, all 9s, alternating 9s / 0, mixed}. Every point (and the first
  past the end) against the view's decimal digits [24x, 24x + 24), and its residue for each of the four primes
  (`p24_point_mod`, the device formula) against the exact value mod p;
- **the ungroup**: coefficients {random < 10⁶⁰, 10⁶⁰ − 1 everywhere, mixed near the bound and small, k·(10²⁴ − 1)²} over whole
  planes (R 16–64, nr 3–8). The host model of `k_crt24`'s stripes gives the out-runs placed by `p24_seq_limb`, plus the 4-limb
  spills at L(R j + a'), against Σ c_x 10^(24x) (GMP);
- **products end to end**: limbs → points → the exact convolution (GMP) → the stripes → limbs, against A·B, from 1 × 1 to
  100 × 33 limbs, random and all 9s;
- **the device part** (hipcc build): `k_crt24` against the host model on R 1024/2048, C 8–16, nr 1, 2, 3, 8 and 12 (rows 85–1024:
  one to four chunks, the unequal parts). The coefficient kinds are random, 10⁶⁰ − 1, near the bound or tiny, and zero. It also
  checks the guard: 10⁶¹ at an s = 12 point must raise flag 1.

| run | command | result |
|---|---|---|
| host, local (gcc 13) | `gcc -O2 -Wall -I. tests/t_p24.c -o t_p24 -lgmp -lm && ./t_p24` | **VERIFY OK (78541 checks)**, 1.7 s (measured) |
| mutation check | seven one-token bugs in p24.h (the point floor, the digit shift's carry, the in-run's extra limb, the spill offset bound, the s = 6 split, the spill-column edge, seq_start's clamp) | each one: VERIFY FAILED (12–39050 checks) |
| device (s24-30, job 21761, 18:22 EDT) | `srun --jobid=$J ... ./tests/t_p24` (the hipcc build) | **VERIFY OK (78594 checks)**: the host part and 53 kernel checks, the guard included; 7 s (measured) |

#### 3. Stage 3: the mn tier behind `MN_P24` (off by default)

##### 3.1 The code (commits 05d84e0, a041fc1, b7eed11, 71a902f)

- **New files:**
  - `ecalc/p24_mn.h`: `k_pack_mn24`, `k_gather_mn24` and `k_scatter_mn24`, included by rns_dist.c after `k_scatter_mn`;
  - `archive:drivers/ecalc/p24_job.sh`: the node batches, unattended.
- **rns_dist.c** (every P24 branch is commented `P24`). Touched: `struct mn_ctx` (+ `p24`), `seg_tables`, `redistribute`,
  `mn_round_add`, `mn_core` (+ the argument `p24`), `mn_grid`, `rns_mul_dist_mn_shape`, `rns_dist_mn_plan`,
  `rns_mul_dist_mn_scratch` and `rns_mul_dist_mn_stage`. New: `mn_run`, `mn_seq_start`, `mn_p24_on`, `mn_p24_of`, `p24_cap`,
  `p24_split` and `p24_grid_shape`. `dist_core` and the prime-count line of `mn_core` (`ec_np_prod`) are unchanged: with P24 off,
  `mn_core` takes the same path, instruction for instruction.
- **Outside my files, minimal and commented:**
  - `dbig.c`: `struct sparse` + `p24`, a P24 branch in `sparse_get`, and `db_share_add_spills_x24`; declared in `dbig.h`;
  - `mn_plan.h`: `struct rns_grid_plan` + `p24`;
  - `Makefile`: `p24_crt.o`, `tests/t_p24` and the dependencies.
- **For agent MPB:**
  - `mn_p24_of` asks `ec_np_for(pa + pb)` of the 18-digit grid's largest piece, the same test `mn_core` makes. If the four-prime
    rule becomes min(pa, pb), that one line should follow it.
  - My `mn_core` edits leave the `ec_np_prod(nc, ...)` call as it was, apart from `p24 ? 4 :` in front of it.

##### 3.2 The tests on one node (s24-30; measured)

| test | command | result | time |
|---|---|---|---|
| t_mn_grid, size 2 | `mnrun.sh 2 env MN_P24=1 ECALC_NP=4 ./tests/t_mn_grid 1 28` | 2 × VERIFY OK (200 checks each) | 180 s |
| t_mn_grid, size 3 (the general map) | the same at 3 | 3 × VERIFY OK | 163 s |
| t_mn_grid, size 4 (with the level-1 subgroups) | the same at 4 | 4 × VERIFY OK | 246 s |
| t_mn_grid, the chunked rounds | `... MN_P24=1 ECALC_NP=4 MN_T_CHUNK_MB=1 ... t_mn_grid 1 28` at 2 | 2 × VERIFY OK | 216 s |
| t_mn_grid, auto with both counts | `MN_P24=1 ECALC_NP=auto ECALC_NP_AUTO_TERMS=1000000` at 2 | 2 × VERIFY OK | 178 s |
| t_mn_grid, auto, every product at P24 | `MN_P24=2 ECALC_NP=auto ECALC_NP_AUTO_TERMS=1000000` at 2 | 2 × VERIFY OK | 178 s |
| 10⁸ at size 2, verbose | `mnrun.sh 2 env MN_P24=1 ECALC_NP=4 POOL_LOG=27 RNS_VERBOSE=1 ./ecalc 100000000` | **identical**, VERIFY OK on both nodes; 72 `P24` lines (18 piece lines on node 0), e.g. 2 653 665 + 2 901 896 limbs on a 2²² plane (18 digits: 2²³) | — |
| mnaccept, P24 at four primes | `MN_P24=1 ECALC_NP=4 ./mnaccept.sh $J --only unit,mn` | **13 passed, 0 failed**: the unit tests (t_mn_grid at 2 under P24), 10⁸ at sizes 2, 3 and 4 and 10⁹ at sizes 2 and 4, all identical | 1253 s |
| mnaccept, every mn product at P24 under auto | `MN_P24=2 ECALC_NP=auto ECALC_NP_AUTO_TERMS=100000000 ./mnaccept.sh $J --only mn` | **5 passed, 0 failed**: 10⁸ at 2, 3, 4 and 10⁹ at 2, 4, all identical | 78 s |

The mnaccept walls (measured, one node, loopback, 2–4 processes on 4 APUs) are not timing evidence: 10⁹ at size 2 was 21.85 s
(P24 = 1) and 22.67 s (P24 = 2). Stage 3's timing pair (batch 5, 10¹⁰ at size 2, P24 0 against 1) is below.

The logs are in `archive:results_raw_2026-10-19/P2415/` (`job_b1.log`, `job_b23.log`, `grid_*.log`, `t_p24_dev.log`, `e8_verbose.log`, `mnaccept_*.log`).

##### 3.3 The gates with the switch off, the kernel fusion, the timing pair (s24-30; measured)

| test | command | result | time |
|---|---|---|---|
| the protocol's gates, the switch off (job 21770, 19:06 EDT) | `./mnaccept.sh $J --only unit,e9,mn` | **15 passed, 0 failed**: t_ntt, t_mul, t_bs, t_dbig, t_newton, t_verify, t_out, t_mn_grid (2); e9 identical in both bases (11.48 s / 21.43 s); 10⁸ at sizes 2–4 and 10⁹ at 2 and 4 identical | 1311 s |
| 4 × 10¹⁰, size 1, the switch off | `./ecalc 40000000000 $F` (the reference evicted first), `digcmp.sh $F results/e_4e10.out` | **identical**; total 65.03 s | — |
| after `k_gather_mn24_np` (A's four gathers in one pass, 3404444; job 21777, 19:50 EDT) | t_mn_grid at 2 and 3 under `MN_P24=1 ECALC_NP=4`; `MN_P24=1 ECALC_NP=4 ./mnaccept.sh $J --only mn` | 2 + 3 VERIFY OK; **5 passed, 0 failed** (10⁸ at 2–4, 10⁹ at 2 and 4 identical) | 176 + 160 + 89 s |
| **the timing pair**: 10¹⁰ at size 2 (`mnrun.sh 2 env MN_P24=<0,1> ECALC_NP=4 POOL_LOG=29 RNS_VERBOSE=1 ./ecalc 10000000000`) | two jobs, 21774 (before the fusion) and 21777 (after), interleaved 0, 1, 0, 1 | every run: VERIFY OK on both nodes, and the digits **identical** to the first 10¹⁰ digits of results/e_4e10.out | see below |

The timing pair: the walls (measured, s; `total`, with bs and dm):

| run | MN_P24=0 | MN_P24=1 |
|---|---|---|
| job 21774, rep 1 | 154.25 (bs 24.6, dm 119.9) | 99.69 (bs 19.3, dm 72.0) |
| job 21774, rep 2 | 158.96 (25.6, 124.8) | 97.02 (18.9, 69.2) |
| job 21777, rep 1 (after the fusion) | 156.22 (24.7, 122.0) | 95.64 (18.2, 68.3) |
| job 21777, rep 2 | 162.98 (26.2, 127.2) | 98.29 (18.7, 70.7) |
| **mean** | **158.1** | **96.97 after the fusion (97.7 all four)**: −38 % |

`tests/p24_timing.py` sums the `dist_mn` piece lines, and its output explains the gain:
- node 0 forms 34 pieces over 10.0 × 2³⁰ plane points at 18 digits, against 32 pieces over **6.0 × 2³⁰** under P24;
- the 18-digit products at the cap 2³⁰ of 2 × 4 ranks need 2³⁰ planes or grids, and at 24 digits they fit one plane of 2²⁹ or
  2³⁰ (the quadratic effect of §1.1 at this size).

**The per-point price at the same plane (the surcharge the model assumes 1.0).** At 2³⁰ points per piece, one node, loopback,
the seconds per 2³⁰ points of a piece are:
- 18 digits: 10.19, 11.19, 9.85 and 10.21 (mean 10.36);
- P24 before the fusion: 11.12 and 10.79;
- P24 after the fusion: 10.52 and 10.73 (mean 10.62), which is **×1.025** (measured, n = 2 runs × 3 pieces against 4 × 8).

By part (after the fusion, per 2³⁰ points):
- the redistribution with A's gathers: 0.47–0.53 → 0.74–0.77. That is 4/3 more limbs moved plus the heavier gather, and the model
  already prices the limbs;
- the transforms with B's per-prime gathers: 8.66–9.07 → 8.98–9.16;
- the CRT: 0.05 → 0.05;
- the result exchange: unchanged.

At the measured ×1.025 the target rows move by +2.5 to +3.6 s (modelled, `MN_MODEL_P24_F=1.025`):

| 5.1 × 10¹³ | cache 0 | cache 2 |
|---|---|---|
| auto, MN_P24=1 | 321.8 / 356.6 s | 305.9 / 340.7 s |
| auto, MN_P24=2 | 314.6 / 349.4 s | 314.6 / 349.4 s |
| ECALC_NP=4, MN_P24=1 | 320.8 / 355.5 s | 320.8 / 355.5 s |

At 5.55 × 10¹³: auto MN_P24=2 is 347.7 / 382.5 s, and NP 4 MN_P24=1 is 354.9 / 389.8 s. Today's 5.1 × 10¹³ is 406.8 / 426.0 s
(auto, cache 0), and 430.2 / 449.3 s (NP 4, cache 0) or 349.4 / 378.8 s (NP 4, cache 2).

#### 4. Stage 4: the plan and the model with the switch

##### 4.1 The C plan (`MN_PLAN_ONLY`, aac6 login node; measured: the code's own decisions)

Command: `MN_P24=<m> ECALC_NP=<np> COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_PLAN_ONLY=<T>:576 ./ecalc`. Output is in
`archive:results_raw_2026-10-19/P2415/plan_summaries.txt`, and the full plan at 5.1 × 10¹³ (NP 4, P24 1) is in `plan_51e13_np4_p24.txt`.

In the table:
- *node 0* is the critical-path count node 0 sees: tree + recip + div.
- *largest* is the same with each tree level's largest group.
- *P24* is the pieces at 24 digits per point.

Every plan check is OK. The largest P24 piece at 5.1 × 10¹³ is 954 126 413 231 + 485 868 190 797 limbs
= 715 594 809 924 + 364 401 143 098 points: 2⁴⁰, and min 3.6 × 10¹¹ < 10¹².

| T | ECALC_NP | MN_P24 | pieces node 0 (tree / recip / div) | largest | P24 pieces (tree, recip, div) | levels node 0 / largest |
|---|---|---|---|---|---|---|
| 5.1 × 10¹³ | 4 or auto | 0 | 222 (108 / 74 / 40) | 242 | — | 8/12 ×4, 12/12 ×2, 32/36, 20/20 |
| 5.1 × 10¹³ | 4 | 1 | **158** (72 / 63 / 23) | **170** | all | 6/8 ×4, 8/8 ×2, 20/24, 12/12 |
| 5.1 × 10¹³ | auto | 1 | 166 (80 / 63 / 23) | 186 | 48 of 80, 24 of 63, 23 of 23 | 8/12 ×4, 8/8 ×2, 20/24, 12/12 |
| 5.1 × 10¹³ | auto | 2 | **158** (72 / 63 / 23) | **170** | all | as NP 4 |
| 5.55 × 10¹³ | 4 or auto | 0 | 259 (132 / 81 / 46) | 293 | — | 8/16, 12/16 ×5, 40/46, 24/24 |
| 5.55 × 10¹³ | 4 | 1 | **180** (86 / 66 / 28) | **182** | all | 6/8, 8/8 ×5, 24/24, 16/16 |
| 5.55 × 10¹³ | auto | 1 | 194 (100 / 66 / 28) | 214 | — | 8/16, 12/16 ×3, 8/8 ×2, 24/24, 16/16 |
| 5.55 × 10¹³ | auto | 2 | **180** | **182** | all | as NP 4 |
| 5.7 × 10¹³ | 4 / auto | 0 | 266 | 292 | — | |
| 5.7 × 10¹³ | 4 | 1 | 182 | 182 | all | 8/8 ×6, 24/24, 16/16 |
| 5.7 × 10¹³ | auto | 1 / 2 | 198 / 182 | 214 / 182 | | |

The model (`mn_model.plan`, node 0) gives the same counts at every row it prices (158, 166, 222; 180, 194, 259):
`archive:results_raw_2026-10-19/P2415/model_target.txt`.

**Memory (C layout, measured on the login node).** `MN_P24=<m> ECALC_NP=auto COMM_SHMEM_ROUND_MB=1024
BS_LAYOUT_ONLY=88541666666.6667:576 ./ecalc 51000000000000 /dev/null`:
- MN_P24=1 raises the dm need from 74.233 to 74.267 GB and the tree need from 66.047 to 66.083 GB (+34 / +36 MB, from the P24
  branch of `rns_mul_dist_mn_scratch`);
- the arena stays 300.65 GB (it is set by the bs regions), and the node is unchanged;
- the pools are unchanged by construction (a P24 plane is at most the cap's q).

The layout does not see MN_P24=2. That mode's decision reads `rns_pool0_np()`, which reads `COMM_SIZE`, and BS_LAYOUT_ONLY does
not set it, so the layout shows mode 1's pattern less the three-prime products. This is an open item; the difference is at most
the same +0.04 GB.

##### 4.2 The estimate (mn_model's P24 term; modelled; the fabric assumed; the write at 0.6 GB/s)

The table in §1.4 stands (the C plan confirms its piece counts). The sweep under auto at cache 0 is
`python3 ../tests/p24_model.py sweep 5.0e13 5.8e13 0.1e13` → `archive:results_raw_2026-10-19/P2415/model_sweep.txt`. Each cell is the wall without the
write / with the write, then node 0's pieces.

| T | today | MN_P24=1 | MN_P24=2 | node (mem_model) |
|---|---|---|---|---|
| 5.0 × 10¹³ | 399.1 / 419.1, 219 | 316.5 / 350.6, 170 | 308.3 / 342.4, 154 | 452.5 GB |
| 5.1 × 10¹³ | 406.8 / 426.0, 222 | 319.3 / 354.4, 166 | **311.0 / 346.2, 158** | 455.4 |
| 5.2 × 10¹³ | 455.5 / 475.8, 230 | 351.0 / 384.2, 175 | 322.2 / 355.4, 165 | 460.5 |
| 5.3 × 10¹³ | 471.3 / 492.6, 242 | 363.1 / 395.6, 184 | 334.3 / 366.7, 170 | 465.6 |
| 5.4 × 10¹³ | 489.4 / 506.1, 251 | 370.3 / 403.8, 190 | 342.4 / 375.9, 178 | 470.6 |
| 5.5 × 10¹³ | 491.0 / 508.9, 259 | 371.9 / 406.6, 194 | **343.9 / 378.7, 180** | 476.0 |
| 5.6 × 10¹³ | 494.2 / 513.1, 261 | 376.0 / 411.8, 198 | 348.1 / 383.9, 180 | 481.4 (over 480) |
| 5.7 × 10¹³ | 496.4 / 516.5, 266 | 376.5 / 413.4, 198 | 348.5 / 385.5, 182 | 486.9 |
| 5.8 × 10¹³ | 534.6 / 549.9, 274 | 418.9 / 457.0, 200 | 406.8 / 444.9, 184 | 492.4 |

**Reading (modelled).**
- At 5.1 × 10¹³ under auto at cache 0 (SC's likely baseline), MN_P24=2 is −95.8 s without the write and −79.8 s with it.
  MN_P24=1 is −87.5 / −71.6 s.
- Under `ECALC_NP=4` at cache 0 (today's launch line), P24 is −113.0 / −97.0 s. With the two cache slots it is −32.2 / −26.5 s.
- 5.55 × 10¹³ under MN_P24=2 (≈ 343.9 s, 180 pieces) is faster than today's 5.1 × 10¹³ at cache 0 (406.8 s), as SC found. The
  memory ceiling (480 GB at ≈ 5.57 × 10¹³, mem_model) then binds.
- The per-point surcharge of the P24 kernels is **assumed** 1.0 in these rows. At 1.10 every P24 row adds 10–16 s (§1.4).

#### 5. Summary, open issues

**For the user's decision.** `MN_P24` is built, off by default, and byte-identical in every test. Adopting it at the target means:
- `MN_P24=1` on the launch line with `ECALC_NP=4` (today's line). Every mn product then runs P24.
- Or `MN_P24=2` with `ECALC_NP=auto`, which gives the same pieces and adds the three-prime leaf.
- At cache 0 either is ≈ 311–321 s against 406.8 / 430.2 s without the write (modelled, surcharge 1.025 measured).
- The target can then be re-chosen near 5.55 × 10¹³ (≈ 348–355 s, modelled; memory 478.7 GB).

**Open issues:**
1. **The transform cache is off for P24 products.** A P24 slot is the same planes (q per prime), so the cache could hold P24
   transforms keyed apart from the 18-digit ones. The cache does not fit at the target anyway (TC), so this was not built.
2. **mem_model.py (not my file) has no P24 sizing.** The C layout at 5.1 × 10¹³ measures +34 MB (dm need) and +36 MB (tree need)
   with MN_P24=1, and an unchanged arena and node. `mem_model --check-c` is exact at the defaults (untouched).
3. **The layout does not see MN_P24=2** (the decision reads `rns_pool0_np()`, which reads `COMM_SIZE`, and BS_LAYOUT_ONLY does not
   set it). The plan does see it. This is harmless for the layout (at most +0.04 GB).
4. **The surcharge is measured at one configuration**: 2 node-processes on one node, loopback, 2³⁰-point pieces, ×1.025, n = 2.
   The target's (2³⁶ points per APU) is assumed to be the same.
5. **B's gathers run per prime** (xb is one plane). A fused B gather would need four B planes; pool 1 has one.
6. **MPB's four-prime rule.** `mn_p24_of` follows `ec_np_for(pa + pb)` of the 18-digit grid's largest piece; if min(pa, pb) is
   adopted, the one line in `mn_p24_of` should follow it. Under `ECALC_NP=4` or `MN_P24=2` the rule does not matter.
7. **Not run**: 10¹¹ at size 1 (the full step), stress, and ckpt / recheck with P24 on. With P24 off, the code path is the same
   as before, apart from the new argument.

**Outside my files** (minimal, commented):
- `ecalc/dbig.c`: `struct sparse.p24`, `sparse_get`'s P24 branch, `db_share_add_spills_x24`;
- `ecalc/dbig.h`: its declaration;
- `ecalc/mn_plan.h`: `rns_grid_plan.p24`;
- `ecalc/Makefile`: `p24_crt.o`, `tests/t_p24` and the dependencies.

#### RESUME

- **Done: stages 1–4.** They cover the design note (§1), the unit test on the host and the device (§2), and the mn tier behind
  `MN_P24` (§3): t_mn_grid at 2, 3 and 4, the chunked rounds, auto with modes 1 and 2, mnaccept `mn` identical, the switch-off
  gates, 4 × 10¹⁰ identical, and the 10¹⁰ timing pair. The C plan and the model are at 5.1, 5.55 and 5.7 × 10¹³ (§4).
- The aac6 clone `~/ntt-P2415` is at 3404444 (this branch before the report commits). Job script: `archive:drivers/ecalc/p24_job.sh <batches>`,
  launched from the login node with `setsid nohup`. Batch 6 re-checks correctness after a kernel change.
- Possible follow-ups (not asked): the P24 cache slot (open issue 1), mem_model's P24 term (open issue 2), and a 10¹¹ size-1
  gate with P24 off.

## NP15 — NP (Phase 15 Batch 2, PLAN §37 row 1) — four primes only where needed (`ECALC_NP=auto`)

(former file: NP15.md)

Branch `p15-NP` from `main` f184d51 (= B1). aac6 clone `~/ntt-NP15`. Times Eastern (aac6 logs are Central: +1 h). Every number is
labelled **measured**, **modelled** or **plan** (the C code's own `MN_PLAN_ONLY` decisions: exact for the code, predicted operand sizes).

#### 1. Design

**The switch**: `ECALC_NP=auto` (off by default: unset, `ecalc.c` still sets 3 for decimal and 4 for binary limbs).

- `crt.c` `ec_np_init`: `auto` with decimal limbs sets `ec_np = 3` and `ec_np_auto = 1`; with binary limbs `ec_np = 4` (four primes
  everywhere, `ec_np_auto = 0`). `ec_np_auto_terms` = the three-prime bound `ec_np3_max_terms` = 58 424 467 928, or the test knob
  `ECALC_NP_AUTO_TERMS=<n>` (1 … the bound; above it the run stops).
- `ec_np_for(nterms)` = 4 when `ec_np_auto` and nterms > `ec_np_auto_terms`, else `ec_np`; `ec_np_prod` = the same plus the old
  `ec_np_check` at three primes. `ec_np_planes(pool_log, g)` = the planes pool 0 must hold: `ec_np`, or under auto 4 when the largest
  plane of g node-processes, 2^(min(31, pool_log) + ⌊log₂ g⌋) points, exceeds the switch-over (an upper bound of `mn_logn_cap` /
  `dist_cap`: no product of the run can exceed it), else 3.
- **Every one-node tier is unchanged**: `ec_np` is 3, so the batch, mdev, host-CRT tiers and their `ec_np_check` calls run exactly as
  with `ECALC_NP=3`. `rns_plane_limbs()` and `mdev_pts()` see the three-prime pool 0 even when pool 0 is made larger, so their
  decisions are those of `ECALC_NP=3`.
- **`rns_dist.c` `dist_core` and `mn_core`**: `const int np = ec_np_prod(nc, …)` per product (nc = pa + pb, the value the old check
  used); every `ec_np` loop (gathers, forward and inverse transforms, transposes, the planes in pool 0: `np · q` limbs) uses `np`;
  the GPU CRT takes `gconst.np = np` (`k_crt_batch` already had both `dgarner3` and `dgarner4`: the kernel is not changed).
  The transforms' contexts and plans for prime 3 exist already (`rank_init` makes all four; plans are made on first use).
- **The B form** (`b_choose`, `b_core`, `b_planes`, `b_fits`): the product's `np` passed through (B at four primes = one prime per
  APU; B4 only at three).
- **The transform cache** (A1, over shares at size > 1): a slot records the prime count of its planes and a lookup matches it (a
  cached three-prime transform is never read by a four-prime product, and the reverse); the slots are sized at
  `rns_pool0_np()` planes; a product with more primes than the slots hold is not cached (a guard; it cannot happen).
- **`rns_mul.c`**: `rns_pool0_np()` = `ec_np_planes(pool_log, COMM_SIZE)`; `rns_init` makes pool 0 at four planes when it is 4 (the
  same bytes as `ECALC_NP=4`: 16 GiB per APU at 2³¹), at three otherwise (at size 1 the pools are those of `ECALC_NP=3`). The
  `rns_init:` line names the choice.
- **`mn_plan.c`** (`MN_PLAN_ONLY`, `BS_LAYOUT_ONLY`'s check): under auto a piece over the bound is not a refusal (it runs four
  primes); each line carries `, 4 primes` / `, 3 primes`, a grid line the count of its pieces at four; a new `plan primes` line counts
  the pieces at four per phase; the header names the pool's planes; `plan check … ECALC_NP=auto: OK … bound per product (four primes
  over … terms: k of n pieces)`. `rns_dist_mn_plan` / `rns_dist_db_plan` give the piece's `np`, the planes' bytes at it, and
  `formed4`.
- **`binsplit.c`** (outside my files, one line, commented): `binsplit_node_bytes` takes pool 0 at `ec_np_planes(pl, g)` under auto
  (the plane-cap switch, the budget check and `BS_LAYOUT_ONLY`'s `planes:` line).
- **Not changed**: the grids (the cap is in points, so the choice of pieces is the same at three, four and auto; the digits never
  depend on the grid). A cost-aware grid that keeps pieces under the bound would be a further step (open issues).

#### 2. The models (modelled)

- `mem_model.py`: `NP3_MAX_TERMS`, `np_planes(np, g, pool_log)` (the port of `ec_np_planes`); `mem_per_node(np='auto')` sizes pool 0 by it.
  `--check-c` exact (§3).
- `mn_model.py`: `piece_np(dz, nc)` (the port of `ec_np_for`): under a design with `np_auto` a piece of the distributed tiers is priced at
  four primes (`T_PIECE_31_NP[4]`, four transforms' exchanges per operand) when nc > the bound, three otherwise; `Design(np_mn='auto')`:
  at size > 1 `at_g` gives np 3 with `np_auto` (the leaf and the one-node phases at three) and the memory with pool 0 by `np_planes`.
  `NP_AUTO_TERMS` is the model's switch-over.
- `estimate.py` (outside my files, two lines): `--np-mn auto`.

#### 3. Results

##### 3.1 The plan and the layout on the login node (plan; `archive:results-raw/NP15/plan576_np*.txt`, `layout_auto.txt`)

| command | result |
|---|---|
| `ECALC_NP=auto MN_PLAN_ONLY=4.25e13:576 ./ecalc` | rc 0; `pools 16.00 + 12.00 GiB per APU` (pool 0 for 4); `plan primes … pieces at four primes tree 56 of 86, recip 26 of 66, div 28 of 28 \| leaf dist_db 0 of 12, recip single-node chain 0 of 30`; `plan check … ECALC_NP=auto: OK -- 1240 products … bound per product (four primes over 58424467928 terms: 110 of 222 pieces); the longest transform 2^40 of 2^44` |
| the same with `ECALC_NP=4` / `=3` | rc 0 (OK) / rc 3 (REFUSED: 84 products over the bound, the first at tree level 5) — as P15. The grids and the summary line (180 pieces; 182 with each level's largest group) are the same under 3, 4 and auto |
| `ECALC_NP=auto BS_LAYOUT_ONLY=7.38e10:576,1e11:1,1e9:4 ./ecalc 42500000000000 /dev/null` | planes at 2³¹: 576 → **120.88 GB** (= `ECALC_NP=4`; node 387.25 GB), 1e11:1 → **103.70** (= `ECALC_NP=3`; node 392.89), 1e9:4 → 103.70 (= 3); the check lines OK |
| `python3 mem_model.py --check-c layout_auto.txt` (4e10:1, 1e11:1, 7.38e10:576, 1e9:4) | **largest arena difference 0.0000 %; 0 terms not exact**; `mem_per_node(np='auto')` planes 103.70 / 103.70 / 120.88 / 103.70 GB = the C `planes:` lines |

The forced test's bound was chosen with the plan (`ECALC_NP_AUTO_TERMS=k MN_PLAN_ONLY=d:g`, plan): at k = 8 × 10⁶ and 10⁹ digits
size 1: recip 7 of 50 pieces and div 2 of 2 at four; size 2: tree 2 of 2, recip 7 of 20, div 2 of 2; size 4: tree 4 of 4, recip 7 of
20. k = 2 × 10⁶ at 10⁸: recip 5 of 44 (size 1) / 5 of 14 (2, 4). k = 5 × 10⁷ at 10⁹ size 4: tree 2 of 4 (a tree level mixed).

##### 3.2 Batch 2: the forced tests (job 21611, s24-26, 18:21–18:47 EDT; measured; logs `archive:results-raw/NP15/`)

Every run under `ECALC_NP=auto`; "lines" = the `RNS_VERBOSE` product lines tagged `, 4 primes` / `, 3 primes` (the B form's lines
were not tagged in this build: size 1 shows 0 / 0; rerun in batch 5).

| test | result |
|---|---|
| `LIMB_BASE=10 ECALC_NP=auto ./tests/t_crt 24` (part 0: `ec_np_for`, `ec_np_prod`, `ec_np_planes` over pool_log 27–31 × g 1…1024; garner3 = garner4 = v on 20 000 values of up to the bound's terms; at bound + 1 three primes wrap, four hold; then parts 1–2 at three primes) | VERIFY OK (102 checks) |
| the same with `ECALC_NP_AUTO_TERMS=1000000` | VERIFY OK (102) |
| `ECALC_NP=auto ./tests/t_crt 24` (binary: auto = four everywhere) | VERIFY OK (24) |
| `LIMB_BASE=10 … ECALC_NP_AUTO_TERMS=4000000 ./tests/t_dbig 0 x` | VERIFY OK (565) (the lines: B form, untagged) |
| `LIMB_BASE=10 … ECALC_NP_AUTO_TERMS=300000 ./tests/t_newton 20` | VERIFY OK (620) (untagged) |
| `t_mn_grid 1 28` decimal, bound 0.7 × the cap (23 488 102 at 2 and 3 procs, 46 976 204 at 4) | size 2: 2 VERIFY OK, lines 712 at 4 / 60 at 3; size 3: 3 OK, 1068 / 90; size 4: 4 OK, 1440 / 336 |
| `t_mn_grid 1 28` binary (auto = 4), same bounds | sizes 2, 3, 4: all VERIFY OK |
| 10⁹, bound 8 × 10⁶: size 1 / 2 / 4 | **identical** to `ref/e_1000000000.txt` at every size; all nodes VERIFY OK; lines at 4 / 3: (B form) / 22 / 26 / 52 / 40; `total` 14.69 / 24.26 / 17.77 s |
| 10⁸, bound 2 × 10⁶: size 1 / 2 / 3 / 4 | **identical** at every size; lines 18 / 18, 33 / 19, 44 / 20 (sizes 2–4); `total` 12.32 / 8.18 / 8.39 / 8.68 s |
| 10⁹ size 4, bound 5 × 10⁷ | **identical**; lines 24 at 4 / 68 at 3 |

At size 2 the `rns_init:` line reads `plane pool 0 for 4 primes` under the lowered bound; the `dist_mn` lines show both counts on
the same node within one run (e.g. the tree level's `2^25 … over 2 x 4 ranks, 4 primes`).

##### 3.3 Batch 1: the standing regression with the switch on (job 21618, s24-30, 18:47–19:09 EDT; measured)

`ECALC_NP=auto ./mnaccept.sh 21618 --only unit,e9,mn` → **15 passed, 0 failed**: t_ntt 24 (3215 checks), t_mul 20 (189), t_bs (10),
t_dbig 0 (555), t_newton 20 (620), t_verify (334), t_out (1), t_mn_grid at 2 procs (200 checks, both nodes); e9 size 1 decimal
**identical**, `total` 11.68 s; e9 binary (auto = four everywhere) **identical**, 22.72 s; mn e8 at sizes 2, 3, 4 and e9 at 2, 4
**identical**, all nodes VERIFY OK (`total` 8.57 / 8.20 / 8.12 / 21.08 / 15.69 s). The unit tests default to binary limbs (auto = 4:
the four-prime path), so their decimal forms under a lowered bound are §3.2's.

##### 3.4 Batch 3: 10¹¹ on one node, the switch on and off (job 21627, s24-30, 19:09–19:19 EDT; measured)

The reference evicted from the page cache first; the packed output unpacked by `tools/unpack_digits -q <file> | sha1sum` against
`~/V214/e_1e11.sha1` (the sha1 of `results/e_1e11.out`).

| run | `rns_init` | `total` | phases (bs + 10dP + dm) | init | VmHWM | sha1 |
|---|---|---|---|---|---|---|
| `ECALC_NP=auto ./ecalc 100000000000 /tmp/…` | `primes per product (ECALC_NP=auto) … plane pool 0 for 3 primes` | 202.14 s | 75.8 + 0.1 + 104.0 = 180.0 s | 22.0 s | 27.4 GB | 578f5efb… **IDENTICAL** |
| the defaults (`ECALC_NP=3`), same node, next | `three primes (ECALC_NP=3)` | 199.65 s | 75.8 + 0.1 + 104.9 = 180.9 s | 18.5 s | 27.5 GB | 578f5efb… **IDENTICAL** |

**Neutral at 10¹¹, as designed** (every product is within the bound; pool 0 has three planes): the computed phases 180.0 vs 180.9 s;
the 2.5 s of `total` is init (22.0 vs 18.5 s), one run each — the init of the defaults spreads 17.4–22.5 s over the recorded runs
(P15's 17.4 s, RESULTS 86), and at size 1 auto maps exactly the pools of `ECALC_NP=3` (`BS_LAYOUT_ONLY`: 103.70 GB both), so I read it
as run-to-run spread, not a cost (one pair; not a series).

A second pair (job 21631, s24-30, 19:31–19:41 EDT; the logs `e1e11_auto.log` / `e1e11_def.log` are this pair's; the first pair's lines
are in `job2134.log`): auto 201.16 s (phases 180.3, init 20.6), defaults 199.82 s (phases 180.7, init 19.0); both sha1 **IDENTICAL**.
In both pairs auto ran first. **Batch 6 reversed the order** (job 21633, s24-30, 19:42–19:52 EDT): defaults 199.37 s (phases 180.6,
init 18.6), then auto **197.17 s** (phases 179.3, init 17.7); both sha1 **IDENTICAL**. The init difference was the order (the first
10¹¹ run of a job maps slower), not the switch.

| 10¹¹, size 1 (three pairs, same node) | `total` | phases (bs + 10dP + dm) | init |
|---|---|---|---|
| `ECALC_NP=auto` | 202.14 / 201.16 / 197.17 s, mean **200.2 s** | 180.0 / 180.3 / 179.3, mean **179.9 s** | 22.0 / 20.6 / 17.7 |
| defaults (`ECALC_NP=3`) | 199.65 / 199.82 / 199.37 s, mean **199.6 s** | 180.9 / 180.7 / 180.6, mean **180.7 s** | 18.5 / 19.0 / 18.6 |

**The switch is neutral at 10¹¹ on one node** (measured): the phases within 1 s (auto 0.8 s lower), the wall within the order effect;
all six digit files identical to the reference. (The digit file is written in every run: the one wall shown is the wall with the write;
at size 1 the packed writer overlaps the division, T1 0.0 s.)

##### 3.5 Batch 4: the cost of four primes against three, per product (job 21629, s24-30, 19:19–19:25 EDT; measured)

10¹⁰ at size 1 and size 2 (two node-processes on one node, TCP loopback, POOL_LOG 29), `RNS_STRATEGY=C RNS_VERBOSE=1`, under
`ECALC_NP=auto` twice: the real bound (every distributed product at three primes) and `ECALC_NP_AUTO_TERMS=1` (every one at four).
All four runs VERIFY OK with **the same sha1** of the unpacked digits (3fc02547…). `python3 tests/np15_ratio.py <three> <four>`
pairs node 0's product lines in order:

| size | pairs | total, three → four primes | ratio | ntt + crt, three → four | ratio | the largest products |
|---|---|---|---|---|---|---|
| 1 (`dist`, four-step over the four APUs) | 60 | 3.430 → 4.478 s | **1.306** | 2.269 → 2.981 s | **1.314** | 2³⁰–2³¹ limbs: 1.33–1.34 (total), 1.36–1.37 (ntt + crt) |
| 2 (`dist_mn`, 2 × 4 ranks) | 45 | 86.14 → 110.96 s | **1.288** | 74.58 → 99.85 s | **1.339** | 2³⁰: 1.27 / 1.32 (8 products, 61 → 77 s) |

The model's price of a piece at four primes against three, `T_PIECE_31_NP` 1.078 / 0.827 = **1.303** (S13, measured on a 2³¹ C product),
is what these runs measure per product (1.29–1.31 total, 1.31–1.34 for the transforms and the CRT). The whole runs: size 1 `total`
27.91 → 30.00 s (dm 6.2 → 7.2), size 2 114.23 → 143.88 s (bs 20.6 → 25.5, dm 84.2 → 108.7). (Small products, below 2²¹ limbs, are
fixed-cost dominated and scatter; at size 2 the pairing drops the lines interleaved from node 1's process.)

##### 3.6 Batch 5: size 1 with the B form's lines tagged (job 21630, s24-30, 19:27–19:31 EDT; measured; build 59371a3+)

| test | result |
|---|---|
| `LIMB_BASE=10 ECALC_NP=auto ECALC_NP_AUTO_TERMS=4000000 RNS_VERBOSE=1 ./tests/t_dbig 0 x` | VERIFY OK (565); lines 4 at four primes, 6 at three (B form) |
| `… ECALC_NP_AUTO_TERMS=300000 ./tests/t_newton 20`, and with `RNS_STRATEGY=C` | VERIFY OK (620) both (no `dist` line printed by this test's products: its size-1 path does not reach the verbose lines) |
| 10⁹ size 1, bound 8 × 10⁶, default strategy (B form) | **identical**; lines 9 at four / 43 at three; `total` 16.27 s |
| the same, `RNS_STRATEGY=C` | **identical**; 9 / 43; 14.03 s |
| 10⁸ size 1, bound 2 × 10⁶ | **identical**; 7 / 39; 13.32 s |

So both counts occur in one run at sizes 1, 2 (and 3) and 4 (§3.2, §3.6), in the B form and in C, and every digit file is identical.

#### 4. The target estimate (modelled; `python3 estimate.py --target --np-mn auto` against `--target` = `ECALC_NP=4`)

mn_model DEFAULT15B (the code's defaults of 2026-09-27 on the target's launch line), TARGET fabric (assumed: 100 GB/s per APU, 2 µs per
message), MN_GROUPS 2,4,8,16,32,64,192,576, the part file packed (0.444 B/digit) from the division's hook:

| digits on 576 | `ECALC_NP` at size > 1 | without the write | with it @0.8 GB/s | @0.6 GB/s | node GB | pieces tree_max + recip + div |
|---|---|---|---|---|---|---|
| **4.25 × 10¹³** (1.2 % below the step) | 4 (the launch line today) | 256.0 s (4.27 min) | 272.0 s | 285.7 s (4.76 min) | 416.0 | 88 + 66 + 28 = 182 |
| **4.25 × 10¹³** | **auto** | **238.6 s (3.98 min)** | **254.6 s** | **268.3 s (4.47 min)** | **416.0** | 182 (the same grids) |
| 4.29 × 10¹³ (the last size below the step) | 4 / auto | 256.4 / 239.0 s | 272.7 / 255.3 s | 286.5 / 269.1 s | 418.1 / 418.1 | 182 |
| 4.30 × 10¹³ (the step: 4.29 → 4.30) | 4 / auto | 271.0 / 251.6 s | 287.5 / 268.0 s | 301.3 / 281.8 s | 418.6 / 418.6 | 194 |
| 4.4 × 10¹³ | 4 / auto | 309.5 / 288.8 s | 322.8 / 302.1 s | 336.9 / 316.3 s | 423.7 / 423.7 | 226 |
| (three primes, not runnable: the plan refuses) | 3 | 212.6 s | 232.5 s | 246.2 s | 398.8 | 182 |

- **−17.4 s at 4.25 × 10¹³** (both walls; 6.8 %), modelled. By phase: the leaf's batch 25.3 → 19.7 and top 19.6 → 16.4 (the one-node
  tiers back at three primes: −8.8 s), the distributed levels 107.0 → 98.7 (−8.3: 56 of 86 tree pieces stay at four, the lower
  levels at three), the reciprocal 33.5 → 33.2 (26 of 66 pieces at four, the rest small), the division 35.5 → 35.5 (every piece at four).
  P15 modelled −20.1 s (277.9 → 257.8) on the model before Batch 1's refit; the same split of pieces (110 at four here, 111 there).
- **Memory: the same as `ECALC_NP=4`** (416.0 GB per node at 4.25 × 10¹³: pool 0 at four planes, 16 GiB per APU), 17.2 GB above three
  primes. **The grid step does not move** (the cap is in points): 4.29 → 4.30 × 10¹³ under 3, 4 and auto (plan and model).
- The per-product price the model uses (1.303 for four against three) is what batch 4 measured (1.29–1.31, §3.5), so the −17.4 s
  rests on a measured ratio; the fabric terms are the model's assumptions as before.
- Memory ceiling (480 GB): the same D per node as `ECALC_NP=4` (9.67 × 10¹⁰ → 5.57 × 10¹³ digits), 384.6 s without the write under
  auto against 411.9 s at four (modelled; past the grid steps).

**The 576-node estimate at this close** (modelled): 4.25 × 10¹³ digits in 238.6 s (3.98 min) without the disk write and 268.3 s
(4.47 min) with it at 0.6 GB/s per node (254.6 s at 0.8) with `ECALC_NP=auto`; 256.0 / 285.7 s with today's launch line (`ECALC_NP=4`);
416 GB per node either way.

#### 5. Files

Own: `ecalc/crt.c`, `ecalc/modarith.h` (declarations), `ecalc/rns_mul.c` / `rns_mul.h` (pool 0, `rns_pool0_np`, the one-node tiers'
view), `ecalc/rns_dist.c` (`dist_core`, `mn_core`, the B form, the transform cache, the plan functions), `ecalc/mn_plan.c` / `mn_plan.h`,
`ecalc/mem_model.py` (`NP3_MAX_TERMS`, `np_planes`, two lines in `mem_per_node`: separable from AS's layout terms), `ecalc/mn_model.py`
(`piece_np`, `Design(np_mn='auto')`), `ecalc/tests/t_crt.c` (part 0), `archive:drivers/ecalc/np15_job.sh`, `tests/np15_ratio.py`, `ecalc/README.md` (two
rows), `results/PRIMES_STRATEGY.md §NP15`, `archive:results-raw/NP15/*`.
Outside my list, minimal and commented: `ecalc/binsplit.c` (one line in `binsplit_node_bytes` + a declaration: pool 0 under auto;
AS owns `dm_layout`, not touched), `ecalc/estimate.py` (`--np-mn auto`: two lines).
`rns_mul.c pick_len` (N3x's) not touched.

#### 6. Open issues

- **Adoption** (the user's): `ECALC_NP=auto` on the target's launch line in place of `ECALC_NP=4` (internal target notes §4,
  internal target task list T3: `MN_PLAN_ONLY` passes it, last line `plan check … ECALC_NP=auto: OK`) — the integrator's files. The code
  default stays unset (3 for decimal limbs).
- Not measured on more than one node: the four-prime products at the target's sizes (2³⁴–2⁴⁰ points) run the same code as the
  two-process loopback runs; P15's `DIST_BIG=34` checked the roots there. The ratio 1.3 is measured at ≤ 2³¹ limbs.
- A grid that prefers pieces under the bound (cost-aware at 1.3× for a four-prime piece) could move more pieces to three primes; the
  grids are unchanged here (the plan and the model agree that the grids and the step are the same under 3, 4 and auto).
- The run's switch-over is pa + pb (the old check's conservative form); min(pa, pb) would move some products to three (46 instead of
  84 over at the target under three primes, P15) — would need the CRT bound argued per product (not done; a larger change).
- `t_newton`'s size-1 products print no verbose line, so its forced run shows correctness (VERIFY OK) but not the counts.
- `BS_LAYOUT_ONLY`'s `planes:` line prints `np 3` under auto (the pool bytes are right: 120.88 GB at 576) — cosmetic, in `binsplit.c`.

#### RESUME

- **Done; the report is final.** Everything committed on `p15-NP` (C code, tests, models, README rows, this report, the node logs in
  `archive:results-raw/NP15/`). aac6 clone `~/ntt-NP15` at 994e43a (built; the later commits are this report and logs only).
- Node jobs (all `-J NP`, 1 node, cancelled by the script): 21611 (batch 2), 21618 (1), 21627 (3), 21629 (4), 21630 (5), 21631 (3
  again), 21633 (6). Nothing is running; no job of mine is queued.
- For the integrator: merge `p15-NP`; the user's decision is `ECALC_NP=auto` on the target's launch line (§4, §6). `estimate.py
  --target --np-mn auto` gives the estimate; `MN_PLAN_ONLY=4.25e13:576` with `ECALC_NP=auto` passes the check.

## MPB15 — MPB (Phase 15 Batch 3): the `ECALC_NP=auto` switch-over on min(pa, pb) (`ECALC_NP_AUTO_MIN=1`)

(former file: MPB15.md)

Branch `p15-MPB` from `int15g` be2eec3 (the worktree HEAD was e107cd2, so I branched from be2eec3). aac6 clone `~/ntt-MPB15`.
Times are Eastern (aac6 logs are Central: +1 h). Every number is labelled **measured**, **modelled**, or **plan** (the C code's own
`MN_PLAN_ONLY` decisions, which are exact for the code at predicted operand sizes).

#### 1. The change (off by default)

**The switch**: `ECALC_NP_AUTO_MIN=1`. It only acts with `ECALC_NP=auto` and decimal limbs. Unset, `0`, `ECALC_NP=3`/`4` or binary
limbs: nothing changes. A `MN_PLAN_ONLY=5.1e13:576` plan with `ECALC_NP=4` or `=3` gives an md5-identical output with the switch on
and off.

- **`crt.c`** reads the switch in `ec_np_init` (the `auto` branch). The new `size_t ec_np_terms(nc, na, nb)` returns min(na, nb)
  with the switch on and `nc` otherwise. **`modarith.h`** has the declaration and the argument (§2).
- **`rns_dist.c`**. These are the lines I touched (P24 edits other mn paths in this file at the same time):
  - `dist_core`: `const int np = ec_np_prod(ec_np_terms(nc, A.n, B.n), bi_decimal, "dist_core");` (the old argument was `nc`). The B form
    takes this `np` through `b_choose` / `b_core` (not changed).
  - `mn_core`: `const int np = ec_np_prod(ec_np_terms(nc, na, nb), bi_decimal, "mn_core");` (the old argument was `nc`).
  - The plan functions, so that `MN_PLAN_ONLY` counts what the run does:
    - `plan_pieces`: `p->formed4 += ec_np_for(ec_np_terms(la + lb, la, lb)) == 4`
    - `rns_dist_db_plan`: two lines, `p->formed4` for one plane and `p->np`
    - `rns_dist_mn_plan`: two lines, `p->formed4` for one plane and `p->np`
  - **Not touched**: `b_fits` (the grid's cost estimate still takes `ec_np_for(pa + pb)`). This keeps the grids unchanged. It is
    conservative: B at four planes fits wherever B or B4 at three fits, and under `auto` `b_choose` checks the placement again at the
    product's own count. It never matters at the target, where the size-1 leaf products are ≤ 2³¹ ≪ the bound.
- **`mn_plan.c`**: a helper `np_by()`. With the switch on, the header's `primes per product (…)`, the `plan primes` line and the
  `plan check … bound per product (…)` text gain ` by min(pa, pb) (ECALC_NP_AUTO_MIN=1)`. With the switch off the output is
  byte-identical to before.
- **`mn_model.py`**: `piece_np(dz, nc, na, nb)` uses min(na, nb) when `NP_AUTO_MIN` is set. `NP_AUTO_MIN` comes from the same
  environment variable `ECALC_NP_AUTO_MIN` (so `ECALC_NP_AUTO_MIN=1 python3 estimate.py --target --np-mn auto` works, and `estimate.py`
  is not edited). The two callers pass the operand lengths. The product memo key includes the flag, and `Design.env()` names it.
- **`tests/t_crt.c`** part 0: `ec_np_terms` checks with the switch on and with it off.
- **`ecalc/README.md`**: one row, `ECALC_NP_AUTO_MIN`.
- New files: `archive:drivers/ecalc/mpb15_job.sh` (the node batches) and `tests/mpb15_lines.py`. The script checks every `RNS_VERBOSE` product line
  against the rule "four primes iff min(na, nb) > k" and counts the products the switch moved (min ≤ k < na + nb, at three).
- Pool 0's size (`ec_np_planes`) is unchanged and conservative. Under min, four planes would be needed only above 2 × the bound of
  plane points. At the target the division's pieces stay at four primes anyway, so pool 0 stays at four planes.

#### 2. Exactness

Take the product C = A·B of operands of na and nb limbs, each limb in [0, 10¹⁸ − 1].

1. **The term count.** The coefficient c_k = Σ a_i b_j over i + j = k, 0 ≤ i < na, 0 ≤ j < nb. For a fixed k, each i gives at most one
   j, and each j gives at most one i. So there are at most min(na, nb) terms. Each term is ≤ (10¹⁸ − 1)², so
   c_k ≤ min(na, nb) · (10¹⁸ − 1)².
2. **No wrap.** Both tiers form the whole acyclic product in a cyclic transform of length n ≥ na + nb.
   - `dist_core`: 2^logn ≥ nc, or 3·2^(logn−1) ≥ nc for r3. The B form's `b_len(nc)` ≥ nc. B4's lo/hi split is an exact
     factorization of the same length-n cyclic product.
   - `mn_core`: `mn_shape`'s 2^logn ≥ nc.
   - Every caller passes nc = na + nb: `rns_mul_dist`, `_hd`, `mul_grid`'s one plane and pieces (pn = ai.n + bj.n), `mn_grid`'s one
     plane and pieces, `b_check`.
   - The gathers zero-fill past each operand's length.

   So each residue class mod n holds exactly one c_k (n ≥ na + nb − 1), and the bound of step 1 holds per transform point.
3. **The cuts and the added operand.**
   - The low cut, w, and the band (`rns_mul_high_db`, `rns_mul_low_db`, `rns_mul_band_db`, mn low / band) only skip whole pieces or
     truncate the operands before the product (`db_view` to w). Every formed piece is a whole product of its two views.
   - The added operand X (`mn_core`'s `cx`) enters `k_crt_batch` as `xk` after Garner, as an exact integer add with a carry. It is not
     a term of the residue sum.
   - A view normalized shorter (top zero limbs) only lowers min.
4. **Three primes.** Garner on p0, p1, p2 returns c mod M, M = p0 p1 p2. So it is exact iff c < M. `np3_max_terms` =
   ⌊(M − 1)/(10¹⁸ − 1)²⌋ = 58 424 467 928 is the largest t with t (10¹⁸ − 1)² < M (its exact 192-bit compare loop). So
   min(na, nb) ≤ bound ⇒ c_k ≤ min · (10¹⁸ − 1)² < M: exact.
   - The switch-over `ec_np_auto_terms` is ≤ the bound (the knob refuses anything above it), so every three-prime product under the
     switch is within the bound.
   - `ec_np_prod` still runs `ec_np_check(min)` on it.
   - `t_crt` part 0 checks garner3 = garner4 = v for values up to bound·(B−1)² and a wrap at bound + 1 (NP's test, run again in §3.2).
5. **The one-node tiers already used min.** `rns_mul_mdev` (`ec_np_check(min(na[0], nb))`) and `rns_mul_batch` (the largest
   min(na, nb)) have always checked min. The switch only brings the distributed tiers to the same rule.

#### 3. Results

##### 3.1 The plan at the target, switch off / on (plan; login node 17:49 EDT; `archive:results-raw/MPB15/plan_<d>_min{0,1}.txt`)

Environment: `ECALC_NP=auto COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_PLAN_ONLY=<d>:576 ./ecalc`,
plus `ECALC_NP_AUTO_MIN=0` / `1`. Every run returned rc 0, `plan check … OK`, 1240 products.

| digits | pieces at four primes, off (pa + pb) | on (min) | all pieces |
|---|---|---|---|
| **5.1 × 10¹³** | tree 76 of 108, recip 35 of 74, div 40 of 40; **151 of 264** | tree **52**, recip **32**, div 40; **124 of 264** | 264 |
| 4.74 × 10¹³ | tree 68 of 100, recip 29 of 68, div 34 of 34 | tree 60, recip 26, div 34 | |
| 4.25 × 10¹³ | tree 56 of 86, recip 26 of 66, div 28 of 28 | tree 48, recip 24, div 28 | |

- **The grids are identical.** All 125 `plan` lines per size agree once the prime tags and the planes' GB are removed. The
  `plan summary` is the same: 222 / 242 pieces at 5.1 × 10¹³.
- **What moves at 5.1 × 10¹³**:
  - Tree level 5's pieces, e.g. 34 326 698 245 + 24 180 462 017 = 5.85 × 10¹⁰ (0.14 % over the bound) with min 2.42 × 10¹⁰: from four
    primes to **three**. The planes go from 68.72 to 51.54 GB per node.
  - Level 6's pieces, and 3 reciprocal pieces.
- **The division stays all at four**, as SC found: its pieces' min is ≫ the bound.

##### 3.2 The node tests (measured)

**Batch 2: the standing regression with the switch on** (job 21763, s24-16, 18:31–18:55 EDT; `archive:results-raw/MPB15/mnaccept_min*.log`)

| command | result |
|---|---|
| `ECALC_NP=auto ECALC_NP_AUTO_MIN=1 ./mnaccept.sh 21763 --only unit,e9,mn` (the real bound) | **15 passed, 0 failed**: t_ntt 24 (3215 checks), t_mul 20 (189), t_bs (10), t_dbig 0 (555), t_newton 20 (620), t_verify (334), t_out (1), t_mn_grid 2 procs (200); e9 decimal **identical** (11.94 s), e9 binary **identical** (20.96 s); mn e8 at sizes 2/3/4 and e9 at 2/4 **identical**, every node VERIFY OK (7.90 / 8.07 / 7.73 / 20.33 / 15.59 s) |
| the same plus `ECALC_NP_AUTO_TERMS=2000000`, `--only e9,mn` (so the switch acts: §3.1-style planning gives both counts and moved products at sizes 1–4, e.g. 10⁸ size 3: tree 4 → 0 of 4 at four primes) | **7 passed, 0 failed**: e9 decimal / binary **identical** (11.99 / 21.41 s); mn e8 at 2/3/4 and e9 at 2/4 **identical**, all nodes VERIFY OK (8.26 / 8.50 / 8.15 / 24.00 / 18.10 s) |

**Batch 1 (the forced tests), first attempt void**: job 21759 (s24-16) was cancelled from outside 75 s after it started (18:15–18:16 EDT;
`sacct`: CANCELLED, not by my script). Only the four t_crt runs had finished: all VERIFY OK (105 / 105 / 103 / 24 checks). Everything
after that failed with an expired job and is not a result. The script now flags a batch whose job ended early (e8d1a67). Rerun as job 21769 (below).

**Batch 1: the forced tests** (job 21769, s24-16, 18:56–19:12 EDT; the job ran to the end; `archive:results-raw/MPB15/job1.log` and the per-test logs).
Every run below has `ECALC_NP=auto ECALC_NP_AUTO_MIN=1`.
- "Lines" are the `RNS_VERBOSE` product lines of every node, checked by `tests/mpb15_lines.py k`. "Moved" = lines at three primes with
  min ≤ k < na + nb: these are the products the old rule ran at four.
- **Against the rule: 0 in every run.**
- The runs of `ecalc` are checked by `digcmp.sh` against `ref/e_<d>.txt`.

| test | result | lines 4 / 3 | moved | `total` |
|---|---|---|---|---|
| `LIMB_BASE=10 … ./tests/t_crt 24` (the real bound) | VERIFY OK (105 checks) | | | |
| the same, `ECALC_NP_AUTO_TERMS=1000000` | VERIFY OK (105) | | | |
| `LIMB_BASE=10 ECALC_NP=auto ./tests/t_crt 24` (switch off) | VERIFY OK (103) | | | |
| `… ./tests/t_crt 24` (binary: four everywhere) | VERIFY OK (24) | | | |
| `LIMB_BASE=10 … ECALC_NP_AUTO_TERMS=4000000 ./tests/t_dbig 0 x` | VERIFY OK (565) | 4 / 6 | 0 | |
| `LIMB_BASE=10 … ECALC_NP_AUTO_TERMS=300000 ./tests/t_newton 20` | VERIFY OK (620) (its size-1 products print no line, as NP found) | 0 / 0 | – | |
| `t_mn_grid 1 28`, size 2, decimal, k 23 488 102 | 2 VERIFY OK | 0 / 772 | **712** (e.g. 20 132 659 × 8 388 608) | |
| the same, size 3 | 3 VERIFY OK | 0 / 1158 | **1068** | |
| the same, size 4, k 46 976 204 | 4 VERIFY OK | 0 / 1776 | **1440** | |
| 10⁹ size 1, k 8 × 10⁶ | **identical**, VERIFY OK | 6 / 46 | 3 (6 944 448 × 3 472 224) | 14.26 s |
| 10⁹ size 2, k 8 × 10⁶ | **identical**, all VERIFY OK | 16 / 92 | 6 | 24.04 s |
| 10⁹ size 4, k 8 × 10⁶ | **identical** | 40 / 172 | 12 | 17.36 s |
| 10⁸ size 1, k 2 × 10⁶ | **identical** | 4 / 42 | 3 | 14.68 s |
| 10⁸ size 2 | **identical** | 12 / 84 | 6 | 8.19 s |
| 10⁸ size 3 | **identical** | 12 / 130 | 21 (a tree level: 1 886 195 × 1 948 658) | 8.46 s |
| 10⁸ size 4 | **identical** | 24 / 160 | 20 | 7.90 s |
| 10⁹ size 3, k 2 × 10⁷ | **identical** | 12 / 150 | 21 (18 819 768 × 19 367 690) | 19.08 s |
| 10⁹ size 4, k 2 × 10⁷ | **identical** | 24 / 188 | 20 | 17.44 s |

What the batch shows:
- Both counts occur in one run at sizes 1–4, in the C form, the B form and the mn tier.
- Every product with min ≤ k < na + nb ran at **three** primes, and every digit file is identical to the reference. That holds for the
  equal-halves tree products too (e.g. 1 886 195 × 1 948 658 at k 2 × 10⁶: pa + pb is 1.9× k).
- In t_mn_grid every product now fits three primes. Under NP's rule 712 / 1068 / 1440 of them were at four (NP15 §3.2).

The switch only lowers each product's prime count, so it can only reduce time. The node timings above are single runs at small
sizes with a lowered bound, where the two rules differ in a few products. I did not run a timing pair; the target's gain is §3.3's
model, on NP's measured 1.29–1.31 per-product ratio.

##### 3.3 The estimate at 5.1 × 10¹³ on 576 (modelled; `archive:results-raw/MPB15/estimate_min{0,1}.txt`)

`[ECALC_NP_AUTO_MIN=1] python3 estimate.py --target --np-mn auto`. Assumptions: mn_model's code defaults, the TARGET fabric (assumed:
100 GB/s per APU, 2 µs), cache 2, the packed part file.

| digits | off: no write / @0.6 GB/s | on: no write / @0.6 GB/s | Δ |
|---|---|---|---|
| **5.1 × 10¹³** | 330.7 / 360.2 s | **324.0 / 353.4 s** | **−6.7 / −6.8 s** |
| 4.74 × 10¹³ | 306.5 / 335.8 s | 299.7 / 329.0 s | −6.8 |
| 4.25 × 10¹³ | 244.8 / 273.0 s | 242.4 / 270.6 s | −2.4 |
| 5.57 × 10¹³ (480 GB ceiling) | 395.3 / 425.7 s | 386.4 / 416.8 s | −8.9 |

- **By phase at 5.1 × 10¹³**: distributed levels 140.8 → 134.2 s, reciprocal 45.3 → 45.1 s, division 53.3 s (unchanged).
- **Memory, pieces and steps are unchanged**: 455.4 GB per node, 242 pieces.
- **This agrees with SC's model at cache 2** (−6.7 s). SC's −9.3 s is its cache-0 figure.

#### 4. Open issues

- Adoption is the user's decision: `ECALC_NP_AUTO_MIN=1` on the target's launch line with `ECALC_NP=auto`.
- `b_fits` still prices the grid by pa + pb. This affects the grid only, never the digits, and is conservative (§1).
- The switch is not applied to `ECALC_NP=3`'s refusal: the plan check and `ec_np_check` still take pa + pb there.
- Pool 0's four planes could be tied to 2 × the bound under min (§1). At the target this does not matter.

#### 5. Files

- **Own**:
  - `ecalc/rns_dist.c`: the seven lines named in §1 (`dist_core`, `mn_core`, `plan_pieces`, `rns_dist_db_plan` ×2, `rns_dist_mn_plan` ×2).
  - `ecalc/mn_plan.c`: `np_by` and three print lines.
  - `ecalc/mn_model.py`: `NP_AUTO_MIN`, `piece_np`, two callers, the memo key, `env()`.
  - `ecalc/README.md`: one row.
  - `results/PRIMES_STRATEGY.md §MPB15` and `archive:results-raw/MPB15/*`.
- **Outside the list, minimal and commented**:
  - `ecalc/crt.c`: the switch in `ec_np_init`, and `ec_np_terms`.
  - `ecalc/modarith.h`: the declarations and the exactness comment.
  - `ecalc/tests/t_crt.c`: part 0, five check lines.
- **New**: `archive:drivers/ecalc/mpb15_job.sh`, `tests/mpb15_lines.py`.
- **Not touched**: `estimate.py` (the model reads the same environment variable), `mem_model.py`, `binsplit.c`, `b_fits`, the grids,
  pool 0.

#### RESUME

- **Done; the report is final.** Everything is committed on `p15-MPB`. The aac6 clone `~/ntt-MPB15` is at e8d1a67 (built; later
  commits are the report and logs only).
- Node jobs (all `-J MPB`, one node, s24-16):
  - 21759: batch 1, cancelled from outside after 75 s; void.
  - 21763: batch 2, mnaccept, passed.
  - 21769: batch 1 rerun, passed.
  - Nothing is running or queued.
- For the integrator: merge `p15-MPB`. Adoption (`ECALC_NP_AUTO_MIN=1` next to `ECALC_NP=auto` on the target's launch line) is the
  user's decision. Estimate with `ECALC_NP_AUTO_MIN=1 python3 estimate.py --target --np-mn auto`.

## P15 — P (Phase 15, PLAN §36, Batch 1 from L8's findings) — roots above 2³³, the prime bound at the 576 target

(former file: P15.md)

Branch `p15-P` from `main` 7367e25 (= B0). aac6 clone `~/ntt-P15`. Times Eastern. Every number is labelled **measured**,
**modelled** or **plan** (the C code's own `MN_PLAN_ONLY` decisions, no device: exact for the code, predicted operand sizes).

#### 0. The three options for 4.25 × 10¹³ digits on 576 nodes (three primes cannot hold the target's pieces)

`tests/p15_options.py` → `archive:results_raw_2026-10-19/P15/options.txt` (**modelled**: `mn_model.py` DEFAULT15 on the TARGET fabric, agent MD's Phase 15
model in main; node memory from `mem_model.py` through `mn_model.memory`; the part file at 0.6 GB/s per node). Grid steps from
`tests/plan_sweep.sh` with `ECALC_NP=3` and `=4` (**plan**, `archive:results_raw_2026-10-19/P15/sweep576_np{3,4}.txt`).

| option | wall without the write | wall with the write (0.6 GB/s) | node memory | pieces (critical path) | 4.25 × 10¹³ below a grid step? | code |
|---|---|---|---|---|---|---|
| three primes (today's defaults) | 231.1 s | 349.6 s | 398.8 GB | 183 | yes (4.29 → 4.30) | **not runnable**: stops at tree level 5 (`ec_np_check`, §2) |
| **(a) `ECALC_NP=4` everywhere** | **277.9 s (+46.8, +20 %)** | **396.3 s** | **416.0 GB** (+17.2: pool 0 at 16 GiB per APU) | 183 | **yes** — the grids are identical at 3 and 4 primes (the step stays at 4.29 → 4.30 × 10¹³) | none: a launch-line setting; `MN_PLAN_ONLY` says OK |
| (b) four primes only for the pieces over the three-prime bound | 257.8 s (+26.7, +12 %) | 376.2 s | 416.0 GB (pool 0 must hold four planes all the same) | 183 | yes (same grids) | not built: ≈ 60–100 lines in `rns_dist.c` / `rns_mul.c` + models + a test knob, ≈ 1 day with tests (§4) |
| (c) three primes, mn piece cap 2³⁵ | 3243 s (14×) | 3361 s | 384.7 GB | 46 472 | no meaning (steps every few 10¹¹ digits) | not built: the code's grid stops at 32 × 32 pieces (`split_grid_cap`); 2³⁴ cap: 9784 s |

At 4.29 × 10¹³ (just below the step): (a) 278.4 / 398.0 s, 418.1 GB; (b) 258.2 / 377.8 s (modelled).
The split of (a)'s +46.8 s: tree levels +21.9, reciprocal +6.6, division +6.9, the one-node leaf phases at four primes the rest (+11.4).
(b) keeps the leaf at three primes and pays four only on the 111 of node 0's 181 pieces that exceed the bound (tree levels 5–8:
56 of 56, reciprocal 27 of 67, division 28 of 28; **plan**). L8's 291.2 vs 244.9 s came from the model before MD's update; the
increment is the same (+46 s).

**Recommendation**: (a) now — it is a launch-line setting (`ECALC_NP=4`), `MN_PLAN_ONLY` passes it, the grids and the step do not
move, and the node stays inside 480 GB (416 GB modelled). (b) is worth −20 s (modelled) for about a day's work and a new tested path
(a per-product prime count in `mn_core`); the user decides. (c) is out. **Fix 1 is needed in every option** (the tree's planes are
2³⁴ from level 3, 8 nodes, up to 2⁴⁰).

**PLAN §36.10's 576-node numbers assume three primes at the 2⁴⁰ cap and are not achievable as modelled**; with (a) the target is
278 s without the write, 396 s with it (4.6 / 6.6 min), 416 GB per node (modelled).

#### 1. Fix 1 — roots of order 2ᵏ and 3·2ᵏ above 2³³ (`modarith.h`)

**The bug** (L8, reproduced): `ec_root(i, logn) = w33^(2^(33 − logn))` — a negative shift for logn > 33; the "root" is 1 for
logn 34 on all four primes (`tests/t_roots` prints the old formula: w = 1, w^(2^33) = 1, **measured** on the login node and a
compute node). `ntt_dist.c plan_create` takes `ec_root(prime, logR + logC)`, reached by the mn tier from 8 nodes on (the target's
tree level 3 forms 2³⁴-point pieces; level 8 and the division 2⁴⁰; **plan**). The same pattern in `ec_root3` (`rns_dist.c
plan3_get`: `ec_root3(prime, logR + logk)`; `ntt3.c`; `rns_dist.c` B form).

**The fix** (no switch; every transform so far bit-identical by construction):
- logn ≤ 33: the table formula exactly as before (same values, same cost);
- 33 < logn ≤ v₂(p − 1): `g^((p − 1) >> logn)` from the generator `ec_G` (w33 = g^((p−1)/2³³), so both are g^((p−1)/2^logn));
  `ec_root3`: `g^(((p − 1) >> logk) / 3)`;
- logn > v₂(p − 1) or < 0: `ec_fatal` (rc 3) naming the prime and its limit. v₂(p − 1) = 48, 47, 44, 44 (the WP8 set c·2⁴⁴ + 1:
  15·2⁴⁸, 27·2⁴⁷, 207·2⁴⁴, 147·2⁴⁴); `ec_logn_limit(np)` = 44 for three or four primes.
- `modarith.h` now includes `fatal.h` (declarations only). `EC_LOGN_MAX` stays 33 (the tables' order), commented as such.

**Call sites** (none changed; they all go through `ec_root` / `ec_root3`): `ntt_dist.c` plan_create (the dist and mn tiers, both
`dist_plan_create` and `dist_plan_create_shared`), `dist_pack_bench`; `rns_dist.c` plan3_get (3·2ᵏ dist planes), the B-form tables
(`b_tables` with `ec_root` / `ec_root3`); `ntt.c` (the local engine's stage tables, ≤ 2³¹; host references); `ntt3.c` (the radix-3
layer); `mn.c` (self-tests via `dist_plan_create`); tests `t_modarith`, `t_ntt`, `t_strategy`.

**Tests**
| test | where | result |
|---|---|---|
| `tests/t_roots` (new, host only: `gcc … -DEC_FATAL_NO_HIP`, in `make`): for all four primes, `ec_root` / `ec_root_inv` / `ec_root3` / `ec_root3_inv` equal the old formula for logn 0…33 (the bit-identity); `ec_root(i, k)` = g^((p−1)/2ᵏ) by GMP `mpz_powm` for k 0…v₂; w^(2ᵏ) = 1 and w^(2^(k−1)) = p − 1 (exact order) for k 1…v₂; the chain root(k)² = root(k − 1) across the 33/34 boundary; root3: GMP, w^(3·2ᵏ) = 1, w^(3·2^(k−1)) = p − 1, w^(2ᵏ) ≠ 1, root3³ = root; `ec_root(i, v₂ + 1)`, `ec_root(i, −1)`, `ec_root3(i, v₂ + 1)` stop with rc 3 (forked child) | local host, aac6 login node, compute node | **VERIFY OK (2789 checks)** (measured); with `-DEC_PRIMES=0` (the old set): VERIFY OK (1176 checks) |
| `t_dist` `DIST_BIG=34` (new mode, `DIST_BIG_PRIMES=0,2`): one 2³⁴-point distributed transform on the four APUs (xGMI, `dist_plan_create` → `ec_root(p, 34)`), 2¹⁷ × 2¹⁷; (1) forward + inverse of a dense plane = identity at every point, (2) the square of a sparse plane (64 random points) against its direct cyclic self-convolution (the slower path, 4096 terms) at every point. The rank's plane in `hipHostMalloc` (32 GiB), the slabs hipMalloc'd (2 × 32 GiB): 96 GiB per APU | node | all four primes: 0 points differ in both checks (§5) |
| `t_dist` `DIST_BIG=33` (the same form at 2³³, the old roots' range: a control of the test itself) | node | prime 0: 0 points differ in both checks (§5) |

2³⁵ does not fit one node (a plane of 256 GiB + two slab buffers of the same size); 2³⁴ is the largest distributed transform above
2³³ one aac6 node holds. The test puts 2³² points on each rank (the production mn tier: ≤ 2²⁹ per rank), so it also exercises the
local engine's batch indexing beyond production sizes.

#### 2. Fix 2a — the plan-time check (`mn_plan.c`, `ecalc.c` BS_LAYOUT_ONLY)

`MN_PLAN_ONLY=<digits>:<g> ./ecalc` checks **every** product it plans — every group of every tree level (not only node 0's, whose
lines it prints), the reciprocal's chain and sharded steps, the division, the leaf's `dist_db` products — against:
- the term bound of `ECALC_NP`: with three primes a piece's pa + pb ≤ `ec_np3_max_terms` = 58 424 467 928 (what `mn_core` /
  `dist_core` pass to `ec_np_check`; binary limbs with three primes refused);
- the roots' 2-adic limit: a transform of 2ᵏ (or 3·2ᵏ) points needs k ≤ `ec_logn_limit(np)` = 44.

It prints one last line, `plan check … OK` or `plan REFUSED …` naming the first product that fails, and exits with 3 on a refusal.
`BS_LAYOUT_ONLY=D:g` (g > 1) runs the same check silently for D × g total digits first (one `plan check` / `plan REFUSED` line) and a
refusal makes its exit status 3 (an `atexit` hook, since `binsplit_layout_only` ends with `exit(0)`; `binsplit.c` not touched).

**Measured on the aac6 login node** (`archive:results_raw_2026-10-19/P15/plan576_np{3,4}.txt`, `layout576_np{3,4}.txt`):

```
ECALC_NP=3 MN_PLAN_ONLY=4.25e13:576 ./ecalc   -> rc 3
plan REFUSED 4.25e+13 digits g 576, ECALC_NP=3: 84 of 1244 products exceed the prime set (84 over the term bound 58424467928 terms
(three primes) as the run checks it, pa + pb; 46 by min(pa, pb); 0 over the roots' 2^44); the first: dist_mn level 5 [0, 32) combine
1 of 1 P (g 32): piece 28579148066 + 30209369707 limbs, 2^36 points: 58788517773 terms > the three-prime bound 58424467928 (1.01x
over); the largest piece 1077046986225 limbs (dist_mn level 8 [0, 576) combine 2 of 2 P (g 576): piece 756030955002 + 321016031223
= 1077046986225 limbs) -- ECALC_NP=4 holds any piece below 2^78 terms
ECALC_NP=4 MN_PLAN_ONLY=4.25e13:576 ./ecalc   -> rc 0
plan check  4.25e+13 digits g 576, ECALC_NP=4: OK -- 1244 products; the largest piece 1077046986225 limbs (...), bound none (4
primes); the longest transform 2^40 of 2^44 (the roots' limit)
ECALC_NP=3 BS_LAYOUT_ONLY=7.38e10:576 ./ecalc 42500000000000 /dev/null   -> rc 3 (plan REFUSED ..., then the layout lines)
ECALC_NP=4 BS_LAYOUT_ONLY=7.38e10:576 ./ecalc 42500000000000 /dev/null   -> rc 0 (plan check ... OK); node 387.25 GB at 2^31 (np 3: 370.07)
```

Other sizes (login node, **plan**): `1e11:1` OK (84 products, the largest piece 2.08 × 10⁹ limbs); `1e9:4` OK; `LIMB_BASE=2
ECALC_NP=3 1e9:4` REFUSED (binary limbs need four primes); `1e14:1024` (three primes) REFUSED, first at tree level 5 (1.08× over);
`ECALC_NP=4 2e14:4096` OK (the longest transform 2⁴³); `ECALC_NP=4 3e15:32768` REFUSED by the roots: tree level 14 forms a 2⁴⁵-point
piece, "a transform of 2^45 points > 2^44 (no root …)", rc 3 — with this prime set the mn tier's cap 2^(31 + ⌊log₂ g⌋) is valid up to groups of 8192 nodes (2⁴⁴); larger groups would need
`mn_logn_cap` clamped at 44 (not done: far beyond the target).

- The first failure is exactly L8's (tree level 5, 32 nodes, 1.01× over). The largest piece is 18.4× over the bound as the run checks
  it (pa + pb) and 8.1× by min(pa, pb) (L8's figure). **Even a check on min(pa, pb) — the coefficients' real term count — leaves 46
  products over**: no cheaper test rescues three primes.
- The summary line (181 pieces, 183 with each level's largest group) is unchanged by the check; it is 2 below internal target notes' 185
  (the reciprocal's cut, NEWTON_RECIP_CUT, now a default).
- The grids are identical at three and four primes (the cap is in points; pool 0 grows with the prime count): the sweep 4.15…4.45 ×
  10¹³ step 10¹¹ shows the same steps (4.25 → 4.26: node 0's tree 86 → 88; 4.29 → 4.30: the largest group 88 → 100; …; 4.39 → 4.40)
  in both (**plan**, `archive:results_raw_2026-10-19/P15/sweep576_np{3,4}.txt`).

**For internal target task list T3 (not my file: for the integrator)**: add `ECALC_NP=4` to the T3 command line and "the last line reads
`plan check … OK`" to its pass criterion.

#### 3. Fix 2b — the options (modelled; §0's table)

- (a) and (b) change no grid: the model's pieces (183) and the plan's are the same at three and four primes, so 4.25 × 10¹³ stays
  1.2 % below the 4.29 → 4.30 step.
- (b) is modelled by pricing each piece at four primes when its pa + pb exceeds the bound and at three otherwise (`p15_options.py`
  patches `mn_model.piece_cost`); the leaf and the one-node tiers stay at three primes. Its memory is (a)'s: the mn tier's planes live
  in pool 0 (`mn_core`: `ec_np · q` limbs), so pool 0 must be sized for four planes at init (16 instead of 12 GiB per APU) — the whole
  of (a)'s +17.2 GB.
- (c) at a 2³⁵ cap (every piece ≤ 3.4 × 10¹⁰ limbs, inside the bound): 46 472 pieces instead of 183 and 3243 s (**modelled**, the grid
  search widened beyond the code's 32 × 32 and the model's 64 × 64). A 2³⁶ plane filled to at most 5.8 × 10¹⁰ limbs (85 %) would need
  ≈ 1/3 of those pieces — still > 1000 s. Not viable.

#### 4. Option (b)'s code, if the user wants it (not built)

- `rns_dist.c mn_core`: a product-local `np` (4 when ec_np = 3 and nc > ec_np3_max_terms, behind a switch, e.g. `ECALC_NP_MN=auto`)
  replacing `ec_np` in its loops (the gathers, the transforms, the transposes), the check, and the plan/ctx for prime 3 (`rank_init`
  must create prime 3's context); the CRT kernel `k_crt_batch` with a four-prime `gconst` (`rns_mul.c`: a second `struct gconst`);
  the transform cache (`cache_slot_bytes` = 4 q; a hit must match the product's prime count);
- `rns_mul.c`: pool 0 at four planes when the switch is on (`rns_plane_pool_bytes`), which is where the memory goes;
- `mn_plan.c` (plane bytes and the check per product), `mem_model.py` / `mn_model.py` (per-product np);
- a test knob lowering the bound (e.g. `ECALC_NP3_MAX_TERMS=<n>`) so `t_mn_grid` / `mnaccept mn` at 10⁸–10⁹ exercise both counts on
  aac6; the digits identical at every size.
≈ 60–100 lines, about a day with the multi-process tests.

#### 5. Node tests (fix 1 in place)

Script `archive:drivers/ecalc/p15_job.sh 21` (unattended, one 1-node job `-J P`, cancelled at the end): the 10¹¹ run and its sha1, then `t_roots`,
`DIST_BIG=33`, `DIST_BIG=34`, `mnaccept.sh --only unit,e9,mn`. Logs in `~/ntt-P15/archive:results_raw_2026-10-19/P15/` on aac6 (copied to `archive:results_raw_2026-10-19/P15/`).

Job 21472 on s24-16, 23:40–00:10 EDT (the clone at fcce434). All **measured**:

| step | command | result |
|---|---|---|
| 10¹¹, size 1, defaults, the reference evicted first | `./ecalc 100000000000 /tmp/p15_e1e11.out` | **VERIFY OK, total 240.06 s** (bs 100.9 + dm 100.5 + T1 21.0; init 17.4; VmHWM 27.3 GB) — in line with 237.7 s of int15's mnaccept (job 21455) |
| its digits | `dd if=… iflag=direct \| sha1sum` against `~/V214/e_1e11.sha1` | **578f5efb0ff2b9af6b681a375c9ff39197f55cb7 = the reference: IDENTICAL** |
| `t_roots` on the node | `./tests/t_roots` | VERIFY OK (2789 checks) |
| 2³³ distributed, four APUs, prime 0 (the control: old-root range) | `DIST_BIG=33 ./tests/t_dist` | identity: 0 of 8 589 934 592 points differ; sparse square vs the direct convolution: 0 differ; transforms 9.6 / 9.5 s |
| **2³⁴ distributed, four APUs, prime 0 (ec_root(0, 34) from the generator)** | `DIST_BIG=34 ./tests/t_dist` | **identity: 0 of 17 179 869 184 points differ; sparse square (64 points, 2080 nonzero products) vs the direct convolution at every point: 0 differ**; transforms 27.3 / 16.0 s, 47.5 s in all |
| mnaccept | `./mnaccept.sh 21472 --only unit,e9,mn` | **13 passed, 0 failed**: t_ntt 24, t_mul 20, t_bs, t_dbig, t_newton 20, t_verify, t_out, t_mn_grid (2 procs); e9 decimal 11.36 s and binary 19.60 s identical; mn e8 at sizes 2, 3, 4 and e9 at 2, 4 identical, all nodes VERIFY OK |

The first `DIST_BIG` run covered prime 0 only: the driver's `strtok` loop was reset by a `strtok` inside the transform setup (fixed:
`strtok_r`, 1f4bc51). Primes 1, 2, 3 at 2³⁴ ran in job 21516 on s24-16, 10:57–11:00 EDT 2026-09-27 (`p15_job.sh 3`; job 21483 the
night before never ran its test: a lost space in the script, fixed in 3f4bae5). All **measured**, `DIST_BIG=34 DIST_BIG_PRIMES=1,2,3
./tests/t_dist` → **VERIFY OK (3 checks)**:

| prime (v₂(p − 1)) | identity (dense), points differing | sparse square vs direct convolution, points differing | transforms |
|---|---|---|---|
| 0 (48), job 21472 | 0 of 17 179 869 184 | 0 | 27.3 + 16.0 s |
| 1 (47) | 0 of 17 179 869 184 | 0 | 32.0 + 21.0 s |
| 2 (44) | 0 of 17 179 869 184 | 0 | 19.0 + 19.9 s |
| 3 (44) | 0 of 17 179 869 184 | 0 | 19.9 + 19.6 s |

**Every prime's 2³⁴ distributed transform is exact with the new roots** (with the old ones each would have used w = 1).

#### 6. Files

Own: `ecalc/modarith.h` (roots), `ecalc/mn_plan.c` / `mn_plan.h` (the check), `ecalc/tests/t_roots.c` (new), `ecalc/Makefile`
(t_roots), `ecalc/tests/t_dist.c` (`DIST_BIG`), `ecalc/README.md` (rows: `MN_PLAN_ONLY`, `BS_LAYOUT_ONLY`, `DIST_BIG`, t_roots),
`archive:drivers/ecalc/p15_job.sh`, `tests/p15_options.py`, `results/PRIMES_STRATEGY.md §P15`, `archive:results_raw_2026-10-19/P15/*`.
Outside my list, minimal: `ecalc/ecalc.c` — the `BS_LAYOUT_ONLY` branch of `plane_cap_switch` calls `mn_plan_check` per point and
an `atexit` hook sets rc 3 (the plan-only path). `crt.c`, `ntt_dist.c`, `rns_dist.c`: not changed (the root call sites need no change;
the checks there stay as they are).

#### Open issues

- internal target task list T3 and internal target notes §4 / §36.10's numbers: `ECALC_NP=4` on the launch line (option (a)) and the new
  last line of `MN_PLAN_ONLY` — the integrator's files.
- The run's own check (`ec_np_check(nc = pa + pb)`) is conservative by up to 2× (the coefficients need min(pa, pb)); tightening it
  would not rescue three primes at the target (46 products still over) and is left alone.
- A prime set with 15 | p − 1 (L8 §3) would change `ec_G` and the tables; the root code derives everything from `ec_G` / v₂ and
  needs no change beyond the constants.

#### RESUME

- Done and committed on `p15-P`: fix 1 (`modarith.h`, `t_roots`), fix 2a (`mn_plan.c` check, `BS_LAYOUT_ONLY`), the options model
  (`tests/p15_options.py`, §0), README rows, `DIST_BIG` test mode, `p15_job.sh`. Login-node results in `archive:results_raw_2026-10-19/P15/`.
- Done: job 21472 (all gates passed, §5; logs in `archive:results_raw_2026-10-19/P15/`).
- Done: job 21516 (primes 1–3 at 2³⁴, §5). Nothing is running; no job of mine is queued. The report is final.

## MEAS-P24 — four-prime product form (P24) for all multi-node products against the default, 2 nodes

(former file: MEAS-P24.md)

**Question.** OPPORTUNITIES AUD3-6: time and device-memory cost of running P24 for every multi-node product (what `tools/gate.sh --p24` forces) at the standard 2-node size.
**Change / branch.** None (measurement only). Build `main` a6ba51d (SHMEM). Switch `ECALC_NP_AUTO_TERMS=1000000` (B), unset (A).
**Method.** Queue item `83_meas_p24` (user-approved 2026-10-12), one 2-node hold, driver `meas_abba.sh`: 4 rounds of A B B A (cap), 2 x 6.441e10 = 1.2882e11 digits, no digit writes, aac7 launch line from `tools/profile_load.py aac7 --line` (+ `COMM_XSTATS=1 MN_WAIT_STATS=1`). VERIFY OK and rc 0 on all 16 runs; B logs show `plane pool 0 for 4 primes`, A `... for 3 primes`. Digits not compared in these runs (the gate compares them at 10^9, `results/CODE_REDUCTION.md §AUD3`).
**Results (measured).**

| statistic | value |
|---|---|
| A (default) totals | 138.71, 131.98, 133.83, 131.77, 133.14, 130.31, 138.00, 131.68 s (mean 133.7) |
| B (P24 forced) totals | 122.81, 122.93, 124.79, 125.04, 126.29, 125.99, 127.26, 123.33 s (mean 124.8) |
| round differences B - A | -12.47, -7.89, -5.58, -9.55 s |
| **mean difference B - A** | **-8.87 s, 95% CI [-13.48, -4.26], 4 pairs, p 0.009** (stop rule met) |

| stage (mean s) | A | B | B - A |
|---|---|---|---|
| binary splitting | 43.1 | 40.3 | -2.7 |
| distributed division multiply (dm) | 52.9 | 44.4 | -8.5 |
| init | about 34 | about 36.5 | about +2 |

| device memory, pool 0 at `[init]` (per node / per APU) | A | B | B - A |
|---|---|---|---|
| in use | 317.8 GB / 79.5 GB | 352.2 GB / 88.1 GB | **+34.4 GB / +8.6 GB** |
| of which planes | 103.1 / 25.77 GB | 120.3 / 30.07 GB | +17.2 / +4.3 GB |
| of which block pool | 214.7 / 53.69 GB | 231.9 / 57.98 GB | +17.2 / +4.3 GB |

Host RSS unchanged (HWM 28.9 GB). The peak during the run was not extracted; the init layout is the pool-0 size.

**Verdict.** P24 is faster, not equal, at 2 nodes: -8.9 s (6.6 %), from the division multiply (-8.5 s) and binary splitting (-2.7 s), at a cost of +8.6 GB per APU (+34.4 GB per node) of device memory at the init layout, twice the earlier modelled +4 GiB (planes and block pool both grow). Whether the target (which uses P24 from 32 nodes) is memory-bound decides the trade; the user decides adoption for the 2-node test form.
**Open issues.** The saving at the target size is not inferred; peak (not init) device memory not extracted; the 4-node three-prime gate form is unchanged.
**RESUME.** Raw: aac7 `~/meas/P24/{totals.tsv,stats.txt,log/}`. Nothing running.

## S13 — Phase 13a, agent S — E0 (the A/B/C strategy boundary) and E2 (plane cap against grid cost)

(former file: S13.md)

Branch `p13-S` (base `main` @ 203733b). New files only: `ecalc/tests/t_strategy.c` (E0, on a node),
`ecalc/tests/t_cap.c` (E2, arithmetic), their two `Makefile` rules and `TESTS` entries, `results/S13_e0.txt`
(the raw E0 output that `t_cap` reads). No library file touched; defaults unchanged.

#### Headline

1. **For one dependent product, prime-per-APU (B) is the fastest strategy at every size measured, 2²⁰ to 2³²
   points** — 1.24–1.52× faster than the four-step (C) from 2²⁶ to 2³¹, 3.7× at 2³² (where C must grid), and
   1.8–2.1× faster than product-per-APU (A), which uses one APU of four. (measured, P = 4)
2. **The boundary is memory, not time.** B needs 2 n points (16 n bytes) on every APU, C 7 n/4 (14 n bytes),
   A 5 n on one APU (40 n bytes). At the production plane budget (28 GiB/APU = pool 0 + pool 1 at POOL_LOG 31)
   B fits up to 2³⁰ (and 3·2²⁹ with radix-3 planes), C to 2³¹, A to 2²⁹. **B at 2³¹ needs 32 GiB/APU — 4 GiB more
   than today's pools.** (measured bytes; the budget is the present layout)
3. **There is no A→B crossover for a single product** in the range: B wins from 2²⁰ up. A is only for
   batches (≥ 4 independent products). **There is no B→C crossover in time**; C is chosen only when B does not
   fit.
4. **P = 3 (measured, P3's `ECALC_NP=3` library, decimal)** keeps the order: B 0.629 s, C 0.827 s, A 1.165 s
   at 2³¹ (C/B 1.31, was 1.51 at P = 4); C's planes fall to 12 n bytes (24 GiB at 2³¹), B's stay 16 n.
5. **E2**: at the present 2³¹ cap the pipeline's big products (top two tree levels, last three reciprocal
   doublings, the division's two products) take **32.2 s under C at 4 × 10¹⁰**. Lowering the cap to 2³⁰ so every
   piece runs exchange-free under B costs 1.45× the transform points and gives **30.0 s (−7 %)**. Radix-3 B planes
   at a 3·2²⁹ cap give **25.0 s (−22 %)**, and B at the 2³¹ cap with a 32 GiB budget gives **21.4 s (−34 %)**. At 10¹¹
   the lowered 2³⁰ cap *loses* (154 vs 129 s); only the 3·2²⁹ cap (116 s, −10 %) or the 32 GiB budget (86 s, −34 %)
   wins. (modelled from E0's medians; see §4)
6. Variant b (a 128-bit operand-load kernel for B's broadcast) gains nothing (≤ 1 %): B's load is limited by xGMI
   peer-read bandwidth, not by the load kernel.

#### 1. What was measured (`t_strategy`)

One product C = A·B, na = nb = n/2 random limbs (so n points, a 2ᵏ transform), four APUs of one node (s24-16),
median of 5 runs after a warm-up (15 at 2²⁰, 2²²), every strategy's result compared limb for limb with the first
one computed (all n limbs) and at 2²⁶ also with GMP. Operands device-resident as `dbig`s (quartered over the APUs,
as in the dm phase) for B and C, a flat copy on APU 0 for A.

| strategy | built from | planes per APU | exchanges |
|---|---|---|---|
| **A** product-per-APU | `ntt_load` / `ntt_fwd` / `ntt_inv_pw` / `k_crt_batch` on APU 0; primes one after another; P result planes + one B plane (the minimum for a device CRT; the library's batch-local tier is this strategy with 2P planes and L ≤ 2³⁰) | (P+1) n on APU 0 | none |
| **B** prime-per-APU | APU d: prime d — loads both operands from the four quarters (a pull "broadcast" over xGMI), fwd, fwd, inverse with the pointwise fused; then APU d forms quarter d of the result by the CRT reading the four APUs' planes (the gather). Built in the test (the library's `mdev` is this strategy through host staging; there is no device-resident form — that is E4) | 2 n on each of P APUs | 2 xGMI phases, 0 all-to-alls |
| **C** four-step | the library's `rns_mul_dist_db` (the dm phase's product), plane pools made by `rns_init` at exactly the production layout | (P+3) n/4 on each of 4 | 3 P all-to-alls per plane product; the 2×2 grid of 2³¹ planes above 2³¹ |

##### The table (P = 4, binary limbs, measured; wall = median in seconds)

| n | A wall | B wall | C wall | fastest | C/B | A/B | A GiB (APU 0) | B GiB/APU | C GiB/APU (dist_core's planes) | C all-to-alls (GiB sent/APU) | B xGMI GiB/APU | C gather+scatter GiB/APU |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2²⁰ | 0.0011 | 0.0008 | 0.0046 | B | 5.75 | 1.38 | 0.04 | 0.02 | 0.01 | 12 (0.02) | 0.01 | 0.01 |
| 2²¹ | 0.0019 | 0.0012 | 0.0034 | B | 2.83 | 1.58 | 0.08 | 0.03 | 0.03 | 12 (0.04) | 0.02 | 0.01 |
| 2²² | 0.0033 | 0.0018 | 0.0042 | B | 2.33 | 1.83 | 0.16 | 0.06 | 0.05 | 12 (0.07) | 0.05 | 0.03 |
| 2²³ | 0.0052 | 0.0032 | 0.0067 | B | 2.09 | 1.62 | 0.31 | 0.12 | 0.11 | 12 (0.14) | 0.09 | 0.06 |
| 2²⁴ | 0.0100 | 0.0057 | 0.0091 | B | 1.60 | 1.75 | 0.62 | 0.25 | 0.22 | 12 (0.28) | 0.19 | 0.12 |
| 2²⁵ | 0.0216 | 0.0128 | 0.0173 | B | 1.35 | 1.69 | 1.25 | 0.50 | 0.44 | 12 (0.56) | 0.38 | 0.23 |
| 2²⁶ | 0.0438 | 0.0238 | 0.0294 | B | 1.24 | 1.84 | 2.50 | 1.00 | 0.88 | 12 (1.12) | 0.75 | 0.47 |
| 2²⁷ | 0.0857 | 0.0453 | 0.0574 | B | 1.27 | 1.89 | 5.00 | 2.00 | 1.75 | 12 (2.25) | 1.50 | 0.94 |
| 2²⁸ | 0.173 | 0.0873 | 0.115 | B | 1.31 | 1.98 | 10.0 | 4.00 | 3.50 | 12 (4.50) | 3.00 | 1.88 |
| 2²⁹ | 0.353 | 0.174 | 0.233 | B | 1.34 | 2.03 | 20.0 | 8.00 | 7.00 | 12 (9.00) | 6.00 | 3.75 |
| 2³⁰ | 0.689 | 0.337 | 0.464 | B | 1.38 | 2.05 | 40.0 ✗ | 16.0 | 14.0 | 12 (18.0) | 12.0 | 7.50 |
| 2³¹ | 1.498 | 0.705 | 1.073 | B | 1.52 | 2.12 | 80.0 ✗ | 32.0 ✗ | 28.0 | 12 (36.0) | 24.0 | 15.0 |
| 2³² | — (160 ✗) | 1.466 | 5.484 | B | 3.74 | — | 160 ✗ | 64.0 ✗ | 28.0 (2×2 grid of 2³¹) | 48 (144) | 48.0 | 60.0 |

✗ = over the 28 GiB/APU plane budget (pool 0 = 2³¹ limbs + pool 1 = `rns_pool1_default_bytes(31)`, the dm phase's
pools). Run-to-run spread (min–max) is ≤ 3 % at every size ≥ 2²³ (the log has it); below 2²³ the numbers are
latency-bound and a first run of 2²² gave B 3.9 ms (a later 15-rep run: 1.8 ms) — treat the 2²⁰–2²² rows as ±50 %.
The A plane bytes, B plane bytes and C pool bytes were read back with `hipMemGetInfo` (dev_delta in the log) and
agree with the formulas; the C column is what `dist_core` takes of its pools (P+3)·n/4 (the pools themselves are
the full 2ᵏ in pool 1 below 2³¹: 8 n/4). hipMalloc of 80 GiB on one APU succeeded (119.8 GiB free on APU 0 at the
start), so the 96 GiB/APU figure in the notes is not a hard cap on this ROCm. Exchange bytes are computed from the
access pattern (not counters): C's all-to-alls send 3/4 of each rank's n/4 points per transform, its gathers read
the operands' rows per prime (3/4 remote) and scatter the result runs; B reads both operands whole (3/4 remote) and
the CRT reads three remote quarter-planes.

**Where the time goes** (s, medians; `load` = operand gather/broadcast, `CRT` = device CRT, `merge` = spills):

| n | strategy | load | transforms | CRT | merge |
|---|---|---|---|---|---|
| 2²⁹ | A | — | 0.322 | 0.031 | 0.000 |
| 2²⁹ | B | 0.065 | 0.078 | 0.033 | 0.001 |
| 2²⁹ | C | 0.051 | 0.154 | 0.019 | 0.007 |
| 2³¹ | A | — | 1.377 | 0.120 | 0.001 |
| 2³¹ | B | 0.252 | 0.330 | 0.129 | 0.001 |
| 2³¹ | C | 0.345 | 0.620 | 0.077 | 0.030 |
| 2³² | B | 0.500 | 0.720 | 0.256 | 0.001 |
| 2³² | C | 1.386 | 2.566 | 0.320 | 0.148 |

C's transform time at 2³¹ is 0.62 s against B's 0.33 s for the same per-APU work: `DIST_STATS` puts 0.44 s of it
(per rank) in the blocked all-to-all (36 GiB sent per APU per product, ≈ 88 GB/s/APU achieved). B's cost is its load:
0.25 s at 2³¹, 24 GiB over xGMI per APU at ≈ 51 GB/s/APU through `ntt_load`'s one-limb-per-thread peer reads — see
variant b below.

**Decimal limbs** (LIMB_BASE=10, the pipeline's base; measured at 2²⁹ and 2³¹): A 0.372 / 1.571, B 0.175 / 0.714,
C 0.238 / 1.090 s — the same ranking and within 2 % except A's larger CRT (decimal conversion in one APU's CRT).

**Nodes.** Every number above is from `aac6-spx-a` (PPAC_MI300A_SPX, job 21004, 2026-09-22). The integrator
offered `aac6-sh5spx-a` (SH5_MI300A_SPX) when an admin CI array took two PPAC nodes; Slurm reports it as **one**
MI300A (`Gres=gpu:1`, 48 CPUs, 120 GB) — not a 4-APU node, so it cannot run B or C (`sbatch --gpus=4` is refused:
"Requested node configuration is not available"). Fell back to PPAC as instructed.

##### Variant b — the broadcast with 128-bit loads (measured, job 21014, s24-26)

| n | B load / wall | b load / wall |
|---|---|---|
| 2²⁶ | 0.0082 / 0.0237 | 0.0080 / 0.0233 |
| 2²⁸ | 0.0311 / 0.0873 | 0.0330 / 0.0878 |
| 2³⁰ | 0.1289 / 0.3443 | 0.1285 / 0.3434 |
| 2³¹ | 0.2591 / 0.7163 | 0.2537 / 0.7117 |
| 2³² | 0.5132 / 1.4851 | 0.5127 / 1.4879 |

No gain: the load is bound by the xGMI pull rate (≈ 51 GB/s per APU on 3/4-remote 64-bit or 128-bit reads). A
push form (each APU writing its quarters to the three others, 697 GB/s node-wide in the Phase-2 bench) would be the
next thing to try for E4; it is not built. The job's C and B rows reproduce the s24-16 ones within 2 % (C 2³¹
1.063 vs 1.073, B 0.716 vs 0.705, C 2³² 5.37 vs 5.48).

#### 2. Crossovers

* **A → B**: none in 2²⁰..2³². B is ahead from 2²⁰ (0.8 vs 1.1 ms) and the ratio grows to 2.1× at 2³¹. A single
  dependent product on one APU leaves three idle; A only competes when there are ≥ 4 independent products (the
  batch tier), which a dependent chain (the reciprocal) never has.
* **B → C**: none in time — C/B is 1.24 at 2²⁶ rising to 1.52 at 2³¹ (the all-to-all volume grows with n, the
  per-product latency floor of the 12 all-to-alls dominates below 2²⁴: C/B = 2–6). C is the choice **only where B
  does not fit**: above 2³⁰ at 28 GiB/APU, above 2³¹ at 32 GiB/APU, and above 2³² everywhere (B at 2³² needs
  64 GiB/APU: fits the HBM, not any pool layout).
* **C → grid**: above 2³¹ (the plane cap) C forms a 2×2 grid of 2³¹ products plus shifted adds: 5.48 s at 2³² vs
  4 × 1.07 = 4.3 s of products — 1.2 s of grid overhead (the temporary, the adds, the zero-padded pieces).

#### 3. P = 3

**MEASURED** on P3's library (`p13-P3` @ 3fbf492, built in `~/ntt-S13-p3`, a worktree of my clone; `t_strategy`
reads the prime count from `ec_np_init()` through a weak reference, so the same source builds on main and on P3's
branch). Job 21014, node **aac6-spx-b**, decimal limbs (`LIMB_BASE=10`, which P = 3 requires), P = 3 and P = 4
in the same job so they compare directly. Every run was VERIFY OK: all strategies agree limb for limb at both P.

| n | P | A wall | B wall | C wall | C/B | A/B | planes GiB/APU A / B / C | C all-to-alls |
|---|---|---|---|---|---|---|---|---|
| 2²⁶ | 3 | 0.0343 | 0.0208 | 0.0220 | 1.06 | 1.65 | 2 / 1 / 0.75 | 9 |
| 2²⁶ | 4 | 0.0453 | 0.0237 | 0.0288 | 1.22 | 1.91 | 2.5 / 1 / 0.88 | 12 |
| 2²⁸ | 3 | 0.134 | 0.0777 | 0.0878 | 1.13 | 1.73 | 8 / 4 / 3 | 9 |
| 2²⁸ | 4 | 0.182 | 0.0881 | 0.114 | 1.30 | 2.06 | 10 / 4 / 3.5 | 12 |
| 2³⁰ | 3 | 0.536 | 0.302 | 0.355 | 1.18 | 1.78 | 32 / 16 / 12 | 9 |
| 2³⁰ | 4 | 0.726 | 0.345 | 0.463 | 1.34 | 2.11 | 40 / 16 / 14 | 12 |
| 2³¹ | 3 | 1.165 | 0.629 | 0.827 | 1.31 | 1.85 | 64 / 32 / 24 | 9 |
| 2³¹ | 4 | 1.581 | 0.714 | 1.078 | 1.51 | 2.21 | 80 / 32 / 28 | 12 |
| 2³² | 3 | — | 1.329 | 4.362 | 3.28 | — | — / 64 / 24 | 36 |
| 2³² | 4 | — | 1.487 | 5.387 | 3.62 | — | — / 64 / 28 | 48 |

P = 3 saves 26 % of A, 23–24 % of C and 12 % of B at 2³⁰–2³¹. B saves less because each busy APU still
transforms a full prime; only its load (0.254 → 0.224 s) and CRT (0.132 → 0.076 s) get cheaper, and APU 3 sits
idle until the CRT. The ranking B < C < A holds at every size, but C closes in (C/B 1.06–1.31, against 1.22–1.51
at P = 4). The model I wrote before the run matched C (0.85 modelled vs 0.83 measured at 2³¹) and overestimated B
(0.71 vs 0.63: it had not taken the cheaper three-prime CRT into account). With P3's pool layout (pool 0 = 3q,
pool 1 = 3q + 16: 24 GiB/APU at 2³¹), B fits 2³⁰ and exactly 3·2²⁹. A form of B that uses the idle fourth APU is
possible but not built: six planes spread over four APUs is 1.5 n points per APU, 24 GiB at 2³¹, which would fit
2³¹ in P3's pools.

#### 4. E2 — `t_cap`: the plane cap against the grid's cost

`t_cap [e0_log] [budget_GiB] [P] [D ...]`: (1) the strategies' plane bytes per APU and the largest plane each fits
in a budget; (2) the pipeline's big products at D digits (nq = D/18 decimal limbs as in `mn_model.py`: the top
two tree levels, the last three reciprocal doublings Q_t·r and r·d, the division's A_h·μ with its low cut and X·Q
mod B^w) under caps 2²⁹, 2³⁰, 3·2²⁹, 2³¹, 3·2³⁰, with the grid `split_grid_cap` chooses (mirrored from
`rns_dist.c`), the pieces formed after the cuts, the transform points and C's all-to-alls; (3) with E0's log, the
time of each shape under each strategy (pieces × E0's median for that plane — decimal medians at 2²⁹ and 2³¹ where
measured, binary elsewhere; a 3·2ᵏ plane = 1.5 × 1.05 × the time of the 2ᵏ⁺¹-point product, the 5 % being
`split_grid_cap`'s own radix-3 penalty — modelled, marked `m`; the grid's shifted adds are not counted).

**The largest plane each strategy fits** (P = 4): at 28 GiB/APU A 2²⁹, B 3·2²⁹, C 2³¹; at 32 GiB A 3·2²⁸,
B **2³¹**, C 2³¹. Needed for 2³¹ / 3·2³⁰ / 2³² planes: A 80/120/160, B 32/48/64, C 28/42/56 GiB/APU.
(P = 3: A 64/96/128, B 32/48/64, C 24/36/48.)

**Totals over those shapes** (s, modelled from measured medians; transform points; C's all-to-alls; which
strategies fit every piece in 28 GiB/APU):

| D | cap | transform points | all-to-alls (C) | fits (28 GiB) | A | B | C |
|---|---|---|---|---|---|---|---|
| 4e10 | 2²⁹ | 1.94e12 | 3612 | A B C | 112 | 52.6 | 71.7 |
| 4e10 | 2³⁰ | 1.12e12 | 1044 | B C | 59.9 | **29.3** | 40.4 |
| 4e10 | 3·2²⁹ | 0.89e12 | 576 | B C | 50.0 | **24.4** | 33.6 |
| 4e10 | **2³¹ (today)** | 0.77e12 | 372 | C | 46.9 | 21.4 (needs 32 GiB) | **32.5** |
| 4e10 | 3·2³⁰ | 0.65e12 | 264 | none | 40.2 | 18.5 | 27.7 |
| 1e11 | 2³⁰ | 5.75e12 | 5352 | B C | 307 | 150 | 207 |
| 1e11 | 3·2²⁹ | 4.14e12 | 2568 | B C | 232 | **113** | 156 |
| 1e11 | **2³¹ (today)** | 3.09e12 | 1440 | C | 188 | 85.7 (needs 32 GiB) | **131** |
| 1e11 | 3·2³⁰ | 2.44e12 | 768 | none | 156 | 71.0 | 108 |

(After job 21014, `t_cap` uses the decimal P = 4 medians from s24-26 where they exist, which moves these totals
by ≤ 3 %. The current numbers and the per-shape rows (grid, pieces, plane, time per strategy) are in
`archive:results-raw/loose/S13_e2.txt`: 28 and 32 GiB at P = 4, 24 and 32 GiB at P = 3.)

**P = 3** (measured P = 3 medians at 2²⁶/2²⁸/2³⁰–2³²; the 2²⁹ planes are modelled): at 4e10 C at the 2³¹ cap
takes 24.7 s against B 18.9 s (32 GiB budget) or B 21.9 s at 3·2²⁹ (24 GiB); at 1e11 C 99.2, B 75.5 / 101.7 s. The sums are the big
products only; the pipeline's measured top + reciprocal + division at 4e10 is ≈ 43 s (mn_model PHASES), so the
C/2³¹ row (32.5 s) is the right order.

#### 5. What this implies for E4 and E5 (regime B in the reciprocal)

* **E4 (a device-resident prime-per-APU path for the reciprocal's doublings)**: justified by E0 at every size the
  reciprocal forms — B is 1.3–1.5× faster than today's C per product from 2²⁶ to 2³¹ and needs no all-to-all.
  The boundary E4 should use is the **memory** boundary: B where 2 n points fit the APU's plane budget, C above.
  With today's pools: B for planes ≤ 2³⁰ (≤ 3·2²⁹ with radix-3 planes, which the test has not built — modelled).
* **E5 (the exchange-free grid)**: at the 28 GiB budget, lowering the cap to 2³⁰ is a small win at 4e10
  (−7 %) and a loss at 1e11 (+19 %): the extra pieces cost more than B saves. It only clearly wins with radix-3
  B planes at 3·2²⁹ (−22 % / −10 %). At P = 3 the margins shrink (B/C 0.76–0.89 instead of 0.66–0.78).
* **The better lever is +4 GiB/APU of plane budget** (pool 1 from 3q+16 to 4q at 2³¹ — the paper's layout), which
  lets B run the present 2³¹ grid unchanged: −33 % on these products at both 4e10 and 1e11 (modelled), no extra
  transform points. Whether 16 GB/node can be found is M's memory-ceiling question (TASKS 1.1–1.3).
* B's own cost is dominated by the broadcast (36 % at 2³¹), and variant b shows the load kernel is not the
  limit. An E4 implementation should push the operands (or keep the reciprocal's operands replicated, since r is
  small) rather than pull them.

#### 6. Tests run (all VERIFY OK)

| what | command (from `ecalc/`) | node / job | result |
|---|---|---|---|
| E0, P = 4 binary, 2²⁰–2³² | `./tests/t_strategy <k> 5 ABC` (`TSTRAT_GMP=1` at ≤ 2²⁶; 15 reps at 2²⁰, 2²²) | s24-16 / 21004 | 13 sizes, A/B/C identical, GMP at 2²⁶ |
| E0, P = 4 decimal | `LIMB_BASE=10 DIST_STATS=1 ./tests/t_strategy {29,31}` | s24-16 / 21004 | identical |
| variant b | `STRATS=BbC ./s13_run.sh J b5 26 28 30 31 32` | s24-26 / 21014 | B = b = C |
| P = 3 and 4 decimal on P3's library | `ECALC_NP={3,4} LIMB_BASE=10 ./tests/t_strategy <k> 5 ABCb` in `~/ntt-S13-p3/ecalc`, k = 26 28 30 31 32 | s24-26 / 21014 | identical at every size |
| E2 | `./tests/t_cap ../results/S13_e0.txt {28,32} 4`, `... {24,32} 3` | none (arithmetic) | `archive:results-raw/loose/S13_e2.txt` |
| regression | `./mnaccept.sh 21014 --only unit,e9` on `p13-S` @ 8333b8a | s24-26 / 21014 | **10 passed, 0 failed** (1226 s) |

Raw output: `results/S13_e0.txt` (every STRAT line and the node/commit of each block).

#### 7. Open issues

* Built only 2ᵏ planes in B. The 3·2ᵏ numbers (the 3·2²⁹ cap) are modelled at 1.575× the 2ᵏ⁺¹ time.
* B's broadcast is a pull at ≈ 51 GB/s per APU. The push form has not been measured.
* B at P = 3 leaves one APU idle. Spreading the planes over four APUs (1.5 n per APU) is not built.
* The exchange bytes are computed from the access patterns, not read from hardware counters.
* `t_cap`'s times leave out the grid's shifted adds (about 1.2 s of C's 5.4 s at 2³²) and the transform cache.

#### RESUME

* **Complete.** Every item is done: E0 at P = 4 (2²⁰–2³²) and at P = 3 (measured), variant b, E2, and the
  regression. No job is live or queued (21014 finished and was cancelled at 01:50 CDT).
* Possible follow-ups, for E4: B with a push broadcast, B with 3·2ᵏ planes, and a B form at P = 3 that uses all
  four APUs.

## B13b — Phase 13b, agent B: strategy B in the library, and B4 (PLAN §31, axis S)

(former file: B13b.md)

Branch `p13b-B` (base `p13b-step0` @ c988218). Files touched: `ecalc/rns_dist.c` (the product dispatch and the new
strategy code, nothing in the pool setup), `ecalc/tests/t_strategy.c` (B4 and the library check `V`), `ecalc/README.md`
(two lines in **Switches**). Default unchanged: `RNS_STRATEGY` unset = `C` = today's product.

#### M-RUN SWITCHES

One environment variable, read once per process (size 1; at size > 1 it applies to the node-local products only, see
"Open issues"). Every value gives the same digits (checked below).

| axis S value | environment | what runs |
|---|---|---|
| **C** (default) | `RNS_STRATEGY=C` (or unset) | the four-step over the four APUs, as before |
| **B** | `RNS_STRATEGY=B` | prime-per-APU for every product; planes that do not fit the pools come from an extra per-APU buffer (hipMalloc, kept to the end of the dm phase) |
| **B4** | `RNS_STRATEGY=B4` | B spread over all four APUs (P = 3), 1.5 n points per APU; extra buffer as B, smaller |
| **auto** | `RNS_STRATEGY=auto` | B wherever its planes fit the plane pools as sized at init, else C; never allocates. The grid prefers pieces that fit B (`RNS_STRATEGY_GRID=1`, the default under auto; `=0` keeps C's grid). `RNS_STRATEGY_FORM=B4` makes auto use B4 |

**Recommended for the M-run's strategy rows**: `C` (unset), `RNS_STRATEGY=auto` (the fastest within today's memory),
`RNS_STRATEGY=B4` (every product prime-per-APU at +24 GiB/APU), and optionally `RNS_STRATEGY=B` (+48 GiB/APU, 442 GB node
peak at 4 × 10¹⁰: too close to the 128 GB per APU for 10¹¹ or a larger plane cap). The plane-cap axis K changes the pools
and with them what `auto` fits: the auto rows must be measured at each cap, not carried over.

Each run prints one line at exit: `rns_dist: RNS_STRATEGY=<s>: C <n> products <t> s, B <n> products <t> s, B4 <n> products
<t> s; extra plane memory <x> GiB/APU (allocations <t> s)` — the products and seconds by form, and the extra memory.
`RNS_VERBOSE=1` adds one line per product.

#### 1. B against B4 (hour 0-1): `t_strategy` at P = 3, 2²⁶–2³²

**B4 design.** The product mod xⁿ − 1 splits into the residues mod x^(n/2) − 1 and mod x^(n/2) + 1. In the forward DIF's
bit-reversed output these are the lower and the upper half (the first stage, then two independent n/2 DIFs). APU p < 3
(prime p) transforms the longer operand whole (n points) and the other operand's lower residue y[j] + y[j + n/2]
(n/2 points). APU 3 forms the three primes' upper residues (y[j] − y[j + n/2]) wⁿʲ, reading the operand once, and
transforms them (3 × n/2 points). Then APU p multiplies its upper half by APU 3's plane (a peer read of n/2 points),
inverts, and the CRT runs as in B. Planes: 1.5 n points = 12 n bytes on every APU, C's figure at P = 3. Transforms: 2.5 n
on APUs 0–2 against B's 3 n. For 3·2ᵏ lengths the same split runs inside each third, after ntt3's radix-3 stage.

**Measured** (job 21047, aac6-spx-b, commit d5615ad5; `ECALC_NP=3 LIMB_BASE=10 ./tests/t_strategy <k> 5 ABC4`;
median of 5 after one warm-up, spread min–max ≤ 3 %; every strategy's product identical limb for limb (VERIFY OK)).
Plane GiB per APU are the largest APU's, from hipMemGetInfo around the allocation.

| n | A s | B s | **B4 s** | C s | B4/B | C/B | planes GiB/APU A / B / B4 / C |
|---|---|---|---|---|---|---|---|
| 2²⁶ | 0.0317 | 0.0201 | 0.0207 | 0.0223 | 1.03 | 1.11 | 2 / 1 / 0.75 / 0.75 |
| 2²⁷ | 0.0631 | 0.0384 | 0.0412 | 0.0427 | 1.07 | 1.11 | 4 / 2 / 1.5 / 1.5 |
| 2²⁸ | 0.1263 | 0.0745 | 0.0800 | 0.0868 | 1.07 | 1.17 | 8 / 4 / 3 / 3 |
| 2²⁹ | 0.2578 | 0.1481 | 0.1589 | 0.1750 | 1.07 | 1.18 | 16 / 8 / 6 / 6 |
| 2³⁰ | 0.5085 | 0.2912 | 0.3099 | 0.3524 | 1.06 | 1.21 | 32 / 16 / 12 / 12 |
| 2³¹ | 1.1028 | 0.6083 | 0.6382 | 0.8183 | 1.05 | 1.35 | 64 / 32 / 24 / 24 |
| 2³² | — (no fit) | 1.2708 | 1.3100 | 4.3564 | 1.03 | 3.43 | — / 64 / 48 / 24 (C: 2×2 grid) |

Where the time goes at 2³¹ (s): B load 0.227, transforms 0.312, CRT 0.076. B4 load 0.157, transforms + the peer
pointwise 0.405, CRT 0.076. C load 0.257, transforms 0.456, CRT 0.072, spills 0.030.

**Why B4 loses.** B4 saves half a transform per prime on APUs 0–2, about 0.05 s at 2³¹. It pays a transfer of n/2 points
per prime from APU 3, 8 GiB at 2³¹ over xGMI. At the 51 GB/s pull rate that is about 0.16 s, and it cannot overlap: APU p
has no free plane to receive into until its own transforms are done. A push, at about 81 GB/s per peer from APU 3's three
links, would still cost about 0.1 s, the same as the saving. On this node the fabric does not buy back what B4 saves.

**Decision.** B4 is 3–7 % slower than B, outside the 5 % rule at 2²⁷–2³⁰, and exactly at it at 2³¹. Its memory
advantage (12 n against 16 n bytes) does not change what fits today's pools: pool 0 = 3q and pool 1 = 3q + 16 per APU,
and both forms need one n-point plane in one pool, so they fit the same planes. **B is the form `auto` uses.** Both forms
are built, because B4 is the lean way to run *every* product prime-per-APU. At the 3·2³⁰ top plane, B needs 48 GiB/APU
of planes and B4 36 GiB (modelled from the plane sizes), against 36 GiB of pools at 4 × 10¹⁰.

#### 2. The library path (`rns_dist.c`)

`dist_core` is the single-node product under every size-1 dist product: the top tree levels (`binsplit.c`'s
`rns_mul_dist_db`), the reciprocal's doublings and the division's products (`newton_db.c`: `rns_mul_dist_db`,
`rns_mul_low_db`, `rns_mul_high_db`, all through `mul_grid`). Its first statement now asks `b_choose` for the form. When
the form is B or B4, `b_core` forms the product and returns; otherwise the C code runs unchanged. The grid split, the
low/high cuts and the pieces' shifted adds are untouched: each grid piece is one `dist_core` call and takes its own form.

* **Transform length**: the smaller of 2ᵏ and 3·2ᵏ⁻² that holds the product, at least 2²⁰. This is independent of
  C's `DIST_R3` rule; the digits do not depend on the length.
* **B**: APU p loads both operands (canonical, zero-extended) from wherever their quarters are. It runs `ntt_fwd` /
  `ntt_fwd3` on both, then the inverse with the pointwise fused.
* **B4**: as in §1. `k_bsplit<T>` forms the residues for T = 1 (2ᵏ) or T = 3 (3·2ᵏ, the radix-3 stage folded in), for
  up to three primes per pass over the operand. `k_b4_pw` does the pointwise, reading APU 3's plane.
* **CRT**: stripes of `rows` coefficients, a power of two ≤ 16384 that divides the result's quarter length. APU d
  takes stripes [G d/4, G (d+1)/4) and `k_crt_batch` writes them straight into the result's quarters. The 4-limb stripe
  spills are laid out as the four-step's sparse spills (R = 4 rows; stripe 4j + r is rank r, column j) and added by
  `db_add_spills`: one chunked-carry pass, safe on long carry chains.
* **Planes**: first fit into plane pool 0 and then pool 1, as sized at init. `auto` takes the B form only if everything
  fits. `B` and `B4` put what does not fit into one grow-only hipMalloc buffer per APU. The four APUs allocate it in
  parallel, the M9 accounting counts it as planes, and it is freed by `rns_dist_cache_release()` at the end of the dm
  phase. If the allocation fails, that product runs C, with a message.
* **Stays C**: host (flat) operands or result, a result view at an offset, and the transform cache
  (`RNS_DIST_CACHE` > 0; 0 at size 1 by default).

##### Library check: `t_strategy <k> <reps> CV` (V = the library under C, B, B4, auto, each compared with C)

Six shapes per size: n/2 × n/2; a 3·2ᵏ-length product; an unbalanced one (0.45 n × n/64); a short one (5000 × 7001,
below 2²⁰); views at offsets 999 / 4097 with odd lengths; odd lengths at offsets 3 / 1. Each shape runs under the four
values, and each result is compared limb for limb with C's. **All identical at 2²², 2²⁶, 2²⁹, 2³¹ (22 checks each,
VERIFY OK; job 21047).**

The n/2 × n/2 product through the library (median of 5, s; the test's pools are `rns_init(min(k,31))`'s production
layout at P = 3: 12 + 12 GiB/APU at 2³¹, so auto takes C there and B/B4 use the extra buffer):

| n | library C | library B | library B4 | library auto |
|---|---|---|---|---|
| 2²⁶ | 0.0230 | 0.0171 | 0.0205 | 0.0203 (B4 form then) |
| 2²⁹ | 0.1759 | **0.1275** (−28 %) | 0.1554 | 0.1551 (B4 form then) |
| 2³¹ | 0.8190 | **0.5389** (−34 %) | 0.6368 | 0.8205 (= C: 2³¹ does not fit 12 GiB pools) |

The library's B is faster than the test's B (0.539 against 0.608 s at 2³¹). Its load kernel reads the quarters in
0.105 s against the test's 0.227 s through `ntt_load`. The first B product at 2³¹ took 6.2 s, the one-time mapping of the
extra planes (2 × 16 GiB per APU).

#### 3. The gate: 10⁹ and 4 × 10¹⁰ under each value

Size 1, decimal limbs, P = 3, `NTT_MODMUL=1` (the step-0 defaults). Every run was **identical** to the reference and
VERIFY OK. At 10⁹ the comparison is `cmp` against `ref/e_1000000000.txt`. At 4 × 10¹⁰ it is the file size plus the md5s
of its 40 1-GB segments, against the same for `results/e_4e10.out` (`~/b13b/segmd5.sh`; in batch 2 `cmp` itself).
The reference was evicted from the node's page cache before every 4 × 10¹⁰ run. **One run each (measured)**; the M-run
repeats them. "device peak" is the largest device total over the M9 phase boundaries; the extra planes of B and B4 are
counted as planes. Product seconds come from the exit line; B's and B4's include the in-phase allocation of their extra
planes.

**4 × 10¹⁰** (auto: job 21049, aac6-spx-b, commit ed3048bd; the others: job 21054, aac6-spx-c, commit 05a7730f):

| RNS_STRATEGY | wall s | phases s | bs | recip | dm | init | device peak GB | products by form | extra planes GiB/APU (alloc s) |
|---|---|---|---|---|---|---|---|---|---|
| C (default) | 65.96 | 48.4 | 26.4 | 10.4 | 22.0 | 17.5 | 287.5 | all C | 0 |
| **auto** (form B) | **62.05** | **42.8** | 23.9 | 7.8 | 18.9 | 19.2 | **287.5** | 73 B in 7.39 s + 9 C in 12.16 s | 0 |
| B4 | 68.65 | 46.7 | 29.0 | 8.4 | 17.7 | 21.9 | 377.7 | 82 B4 in 22.36 s | 24 (4.50 s) |
| B | 71.60 | 51.9 | 35.1 | 7.6 | 16.7 | 19.7 | 442.1 | 82 B in 27.09 s | 48 (11.47 s) |

(A first B run, job 21049 on s24-26: 77.21 s, phases 55.7, 14.03 s of allocations; its digits were not compared
because the job ended first.)

* **auto** takes 5.6 s (−12 %) off the phases at no memory cost. It runs 73 of the 82 products in B form. The 9 that
  stay C are the 3·2³⁰-point pieces of the top product and the division, which need 48 GiB/APU in B against the pools'
  18 + 18. The reciprocal falls from 10.4 to 7.8 s (−25 %), and the dm phase from 22.0 to 18.9 s.
* **B** and **B4** run those 9 in their form as well. The reciprocal and the division get faster (dm 16.7 and 17.7 s),
  but the extra planes are mapped inside bs: 11.5 s for 48 GiB/APU, 4.5 s for 24. The mapping costs more than the 9
  products gain. Net of the mapping (modelled: the same run with the planes already mapped), B's phases would be about
  40.4 s at 442 GB and B4's about 42.2 s at 378 GB, against auto's 42.8 s at 287.5 GB. Only pools sized for B at init,
  with the mapping overlapped with the seeds, would make B the fastest row. That is a pool-layout question (agent P /
  the design table), and it costs 154 GB (B) or 90 GB (B4) of node memory.
* The wall numbers compare across two nodes (s24-26, s24-30). The phases are the fairer comparison. Init varies by
  ±2 s from run to run.

**10⁹** (C, B, auto: job 21049; B4 and the checks: job 21054 after the table fix):

| RNS_STRATEGY | total s | dm s | products by form |
|---|---|---|---|
| C | 17.44 | 2.6 | all C |
| B | 16.16 | 1.9 | 52 B in 0.30 s |
| B4 | 16.79 | 2.0 | 52 B4 in 0.37 s |
| auto | 16.25 | 1.9 | 52 B in 0.31 s |
| B4 with `RNS_STRATEGY_CHECK=1` | 18.99 | — | 52 B4 products each formed again by C and compared: all identical (C 1.40 s for the same 52) |
| B with `RNS_STRATEGY_CHECK=1` | 17.81 | — | 52 B products: all identical (C 1.31 s for the same 52) |

At 10⁹ the B forms are 4× faster per product than C (0.30 s against 1.31 s for the same 52 products). At these sizes
C's nine all-to-alls per product are latency.

**Bug found and fixed in batch 2**: B4 hung in the reciprocal at 10⁹, and the 4 × 10¹⁰ B4 run was cancelled. The
cause was the twiddle-table cache (four slots, always evicting the last). A B4 product's two tables (w_m and w_n) then
evicted each other, the first was used after it was freed, the products were wrong, and Newton repeated forever.
The fix: eight slots, evicted round-robin (05a7730f). `RNS_STRATEGY_CHECK=1` now compares every B-form product with C and
aborts with its shape on a difference.

**The grid under auto** (batch 5, job 21062, aac6-spx-c, commit 744df439, all identical by the segment md5s): 9 C
products remained under auto. They were the 3·2³⁰-point pieces that fit C's plane cap but not B's pools. Commit 744df439
lets `split_grid` weigh each candidate grid by the form its pieces will run in: B pieces cost their points × 0.70,
C pieces their points × 1, and 3·2ᵏ lengths × 1.05. Every product then runs B. The top tree level's P and Q products
become 1 × 2 pieces of 2³¹ (1.37 s each). The level below becomes 3 × 1 pieces of 3·2²⁸ (1.0 s). The division's two
products become 2 × 3 pieces of 2³¹ with one skipped (4.34 and 4.70 s). Same node, one run each:

| run | wall | phases | bs | recip | dm | products |
|---|---|---|---|---|---|---|
| C | 69.71 | 48.1 | 26.0 | 10.46 | 22.0 | all C |
| auto, `RNS_STRATEGY_GRID=0` | 64.09 | 45.3 | 25.3 | 8.18 | 19.9 | 73 B in 7.59 s + 9 C in 12.91 s |
| **auto** (grid on, with `RNS_VERBOSE=1`) | 65.37 | **43.6** | 25.1 | 8.24 | 18.4 | **89 B in 17.06 s** |

The grid takes 3.4 s off the products (20.5 → 17.1 s) and 1.7 s off the phases. Against C, auto saves **4.5 s of
phases (−9.4 %)** on this node at the same 287.5 GB peak. The walls are noisy through init (17.5–21.9 s on the same
node), so the phases are the comparison to use. The auto run of batch 2 (s24-26, before the grid) had phases of 42.8 s.
The M-run's repeated runs will settle node-to-node variation.

**Regression**: at the default, `./mnaccept.sh 21057 --only unit` **8 passed, 0 failed** (1158 s) and
`./mnaccept.sh 21054 --only e9` **2 passed, 0 failed** (LIMB_BASE=10 identical 16.92 s, LIMB_BASE=2 identical 22.47 s).
Both ran at commit 05a7730f; the later commit 744df439 changes only the auto grid, which the default does not reach.
Under auto, `RNS_STRATEGY=auto ./mnaccept.sh 21062 --only mn` gave **5 passed, 0 failed**: 10⁸ at sizes 2, 3, 4 and
10⁹ at sizes 2, 4, identical (the node-local products take the strategy). Also 10⁹ under auto with the grid: identical,
and with `RNS_STRATEGY_CHECK=1` all 52 products identical to C.

#### Open issues

* **Size > 1**: the mn tier's products (`rns_mul_dist_mn`: tree levels over node groups, the distributed reciprocal and
  division) stay C. Their operands are shares spread over nodes. A prime-per-APU form there would need the whole operand
  on every APU of a group (a broadcast across nodes, n points per APU from the fabric), which is B's load at fabric
  instead of xGMI bandwidth: 25–50 GB/s per APU against 51. It is not built. The node-local products at size > 1 (the
  node's own leaf tree above the batch tier) do go through `dist_core` and take the strategy.
* The grid (`split_grid_cap`) still chooses pieces by C's cap and cost. A cost that knows B fits 2³¹ but not 3·2³⁰ could
  prefer 2³¹ pieces under `auto`. This is S13's E2 question, not measured here.
* B4's transfer could become a push into the freed lower-residue plane, overlapped with the upper-half forward transform
  if X is also split into residues. Modelled: no gain on this node, where the transfer (≈ 0.1 s at 2³¹) equals the
  saving.

#### RESUME

* **Complete.** Batches 1–5 (jobs 21047, 21049, 21054, 21057, 21062) are done. No job is live or queued.
* Scripts and logs on aac6: `~/b13b_b{1..5}.sh`, `~/b13b/b{1..5}.out`, `~/b13b/b{2,3,5}/*.log`; the reference's
  segment md5s are in `~/b13b/ref4e10.md5`, the regression logs in `~/ntt-B13b/ecalc/results/mnaccept/{21054,21057,21062}/`.
