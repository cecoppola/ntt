# LAUNCH_DEFAULTS — launch-line decisions and defaults adopted

Scope: launch-line decisions and defaults adopted: slot release, K2+K3, P24 default, the pre-launch memory fit, exchange re-confirm. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| B01 | launch-line decisions of 2026-10-11 (branch b01 from main bc80001) |
| L1 | COMM_LAYER_VSLOT_RELEASE=1 on the launch lines (user, 2026-10-11) |
| T08 | K2 + K3 default on (user adopted 2026-10-11) |
| ADOPT2 | `P24_CRT_SCAN=1` (D24) and `NEWTON_CHAIN_GUARD=1` (D25) as code defaults; two helper merges |
| DEF | P24 as the multi-node default (D18) and DM_MN_WRAP in the memory model + 6-node A/B (D19 b) |
| FIT | the memory fit before every launch, and DM_MN_WRAP automatic |
| S47 | exchange settings re-confirm at 4 nodes (aac7, batch 2026-10-11, track T2) |

## B01 -- launch-line decisions of 2026-10-11 (branch b01 from main bc80001)

(former file: B01.md)

Labels: m measured, mod modelled, ass assumed. Times Eastern.

#### Changes (no code default changed)
- Target launch line: `MN_T_CHUNK_MB=2048`, X2 (`COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1`), `ECALC_SEGV_TRACE=1`, `RNS_DIST_CACHE_FIT=1`
  (no `RNS_DIST_CACHE_MN`; the kit's `cache_guard` accepts FIT=1). `target_kit.sh`: the s2chk launch line (`S2_LINE_DEFAULT`) carries all of them;
  `ECALC_SEGV_TRACE=1` also in the uffd, edge (e9, e10) and a4 runs. The kit has no `launch`/`headline` stage yet (hand-off draft item), so the s2chk line is its launch line.
- aac7 line: confirmed `e16_headline.sh` LINE already has T2048 + X2 + FIT (README said so); only stale comments changed ("target line stays 1024"). SEGV_TRACE is set by `tools/gate.sh` always; not added to e16.
- `ECALC_SEGV_TRACE` source check (`segv_trace.c`, `ec_segv_trace_install`): off = one getenv; on = reads host/rank/exe, warms `backtrace`, installs SIGSEGV/SIGBUS/SIGABRT handlers; the handler prints and re-raises with the default action. Nothing else.
- Model: `mn_model.py` `aac7` is now an alias of `aac7_s48` (aac7_s48_fit.json); the previous profile is `aac7_c16` (selectable by name; `refit_s48.py` uses it as its base). `estimate.py` default (no --fabric) is still `target`. 4 nodes, 1.6e11: aac7 107.5 s = aac7_s48; aac7_c16 232.3 s (mod).
- Spares: README and kit help say exclusion only; `--spares N` kept, documented "not used". Private `docs/HANDOFF_DRAFT.md` updated (N = 576 exactly; 4.3 exclusion policy; T2048, X2, SEGV decided; size note).

#### Memory check at 576 nodes (mod unless noted)
Method: `estimate.py --fabric target-m --g 576 --groups 2,4,8,16,32,64,192,576`, `COMM_LAYER_VSLOT_SHARE=1`, with only MN_T_CHUNK_MB set to 2048 and
MDB_SHIFT_CHUNK_MB left at 1024 (a wrapper that sets `Design.t_mb`; the plain `--chunk-mb 2048` also moves the shift chunk and overstates by ~4 GB: 362.1 GB at 3.71e13).
X2 = +0.5 GB (the user's figure; the estimate has no X2 term). Rank 0 = +2.1 GB (S52).

| digits | T1024 device | T2048 device | + X2 0.5 + rank 0 2.1 | margin to 373.44 | node total |
|---|---|---|---|---|---|
| 3.71e13 | 351.7 | 357.9 (+6.2) | 360.5 | **12.9 GB (fits >= 10)** | 402 of 480 |
| 3.82e13 | 357.9 | 366.5 (+8.6) | 369.1 | **4.3 GB (below 10)** | 411 of 480 |
| 3.79e13 | -- | 360.3 | 362.9 | 10.5 GB | -- |
| 3.80e13 | -- | 366.5 | 369.1 | 4.3 GB | -- |

Largest safe size with >= 10 GB rank-0 margin: about 3.79e13 (the model steps between 3.79e13 and 3.80e13; finer than 0.01e13 not tried).
The T2048 step is +6.2 GB in the model (above the +4.3-4.8 GB estimate: the model rounds to the arena chunk of 4.3 GB). The C layout (m, aac7 login node, BS_LAYOUT_ONLY, no OFI pools) shows exactly +4.30 GB `device_with` for T2048 at both sizes (334.70 -> 339.00 at 3.71e13; 339.00 -> 343.29 at 3.82e13).
`MN_PLAN_ONLY=<D>:576` with the full line (T1024 and T2048, X2 on): `plan check ... OK` at 3.71e13 and 3.82e13, 1236 products, SHMEM pool 17664 MB (m, login node).
Decision for the user: put T2048 + X2 on the line (done); 3.82e13 loses the 10 GB rule, 3.79e13 keeps it. Size is the user's later call.

#### Gates (aac7, own jobs, m)
- `tools/gate.sh --build --nodes 1 --alloc` (line aac7 = T2048 + X2 + FIT + ...; SEGV_TRACE=1): PASS, VERIFY OK, digits identical, 1e9, ecalc total 12.50 s (job 1 node, 15:37).
- `tools/gate.sh --nodes 2 --hold 12539` same line: PASS, VERIFY OK, digits identical, 1e9, ecalc total 19.55 s (15:49). Build had `-DCOMM_SHMEM` (gate checks it).
- `target_kit.sh --site aac7 --only build,plan --exclude-hosts aac7-n61` at 2 nodes (hold 12541): plan 2e10 on 2 and 8e10 on 8: plan check OK; status PASS (an `--only plan` alone fails by design: needs kit_modules.env from stage build).
- Jobs cancelled, no steps left, aac7 worktree removed. Raw logs: aac7 ~/sess1011c/b01/.

#### RESUME
Branch b01 pushed, not merged. Open: the size (3.79e13 vs 3.82e13) is the user's; kit has no launch stage; 576-node memory with T2048+X2 is modelled only.

## L1 -- COMM_LAYER_VSLOT_RELEASE=1 on the launch lines (user, 2026-10-11)

(former file: L1.md)

Labels: m measured, mod modelled. Times Eastern. Code default unchanged (0).

#### Changes
- `ecalc/e16_headline.sh` LINE and `ecalc/target_kit.sh` `S2_LINE_DEFAULT` (+ header comment): `COMM_LAYER_VSLOT_RELEASE=1` after `COMM_LAYER_VSLOT_POOL=1`.
- `profiles/aac7.conf`, `profiles/target.conf.template` (evidence comment), `tools/c04_queue_item.sh` (its LINE sanity check), `ecalc/README.md` (new launch-table row; switch row "adopted on the launch lines 2026-10-11 (C06)"), `TASKS.md` (33 adopted; new 39 K7-a GPU-3 rebalance, 40 packed output ECALC_OUT_PACKED=2).
- Private (untracked): `docs/target.conf.draft`, `docs/HANDOFF_DRAFT.md`.
- `tools/profile_check.sh profiles/aac7.conf -q`: 34 keys, MATCH 24, MISMATCH 0, UNKNOWN 0 (m).

#### Memory at 576 nodes with the full target line (T2048 + X2 + RELEASE), C code on the aac7 login node (m)
`MN_PLAN_ONLY=<D>:576` and `BS_LAYOUT_ONLY=<D/576>:576 MEM_REPORT_DEVS=1`, `COMM_OFI_PLAN_CXI=1`, MN_GROUPS=2,4,8,16,32,64,192,576; RELEASE 0 vs 1 (raw: aac7 ~/sess1011c/l1/mem/).
Plan check OK at all three sizes, 1236 products; comm_ofi pool 4352 -> 2304 MiB per APU.

| digits | layout device_with RELEASE=0 -> 1 (m) | delta | rank-0 est. before (B01, mod) | rank-0 est. with RELEASE (mod) | margin to 373.44 GB, rank 0 (mod) |
|---|---|---|---|---|---|
| 3.71e13 | 357.25 -> 348.66 | -8.59 GB | 360.5 | 351.9 | 21.5 GB |
| 3.79e13 | 357.25 -> 348.66 | -8.59 GB | 362.9 | 354.3 | 19.1 GB |
| 3.82e13 | 361.55 -> 352.96 | -8.59 GB | 369.1 | 360.5 | **12.9 GB (above the 10 GB rule again)** |

Method: the est. column is B01's S52-method rank-0 figure (estimate.py with T2048, +0.5 GB X2, +2.1 GB rank 0) minus the measured layout delta (the delta is the pool change only; digits cannot change).

#### Gate
Queue item `~/aac7q/queue/91_l1_gate.sh` (2 nodes, EST 20, STUB dry-run ok, atomic): `tools/gate.sh --build --nodes 2` on the new aac7 line, then `target_kit.sh --only build,plan --nodes-a4 2` (plan needs kit_modules.env, hence build). Output `~/sess1011c/l1/summary.txt` (last line DONE). The queue is busy until ~21:45, so no result at report time.

#### RESUME
Branch l1 pushed, not merged. Collect: `ssh aac7 'cat ~/sess1011c/l1/summary.txt'`; need gate PASS (VERIFY OK, digits identical, -DCOMM_SHMEM) and kit plan check OK. Then merge, remove worktrees (local ../ntt-wt-l1, aac7 ~/ntt-wt/l1), TASKS item 29 can say the 3.82e13 margin is 12.9 GB with RELEASE.

## T08 - K2 + K3 default on (user adopted 2026-10-11)

(former file: T08.md)

Branch `t08` (from main ceb94c1). Not merged.

#### Change
- `ecalc/dbig.c`: `modq_mont_on()` (`DBIG_MODQ_MONT`) and `addsub3_on()` (`DBIG_ADDSUB3`) now default 1 when unset; `=0` turns each off. Comments updated.
- K3 fallback unchanged: `db_mod_qs` uses `k_modq_m` only if every q is odd and in [2^62, 2^63), else the old `k_modq`. K2 still needs `DBIG_ADDSUB2` (default 1) and otherwise falls back to `k_addsub2`.
- K4 `RNS_CRT_SUB` stays off.
- `ecalc/README.md`: two rows added to the "Code defaults that matter" table; Switches-table rows for both marked default 1 since 2026-10-11 (S51 evidence).
- No launch line, script or test outside the local archive and `results/` set these switches explicitly (grep), so none needed editing.

#### Gates (all measured)
aac7, lock B2, 2-node hold (job on 192C4G1H_MI300A_RHEL9, `-x aac7-n61`), worktree build via `tools/gate.sh --build` (SHMEM). Times are the gate's "ecalc total" (single run, not a timing claim).

| gate | command | result | ecalc total |
|---|---|---|---|
| 1 node, new defaults | `tools/gate.sh --build --nodes 1 --hold J` | PASS, VERIFY OK, digits identical, segv 0 | 12.75 s |
| 2 nodes, new defaults | `tools/gate.sh --nodes 2 --hold J` | PASS, VERIFY OK, digits identical, segv 0 | 19.67 s |
| 1 node, old path | `tools/gate.sh --nodes 1 --hold J --env "DBIG_ADDSUB3=0 DBIG_MODQ_MONT=0"` | PASS, VERIFY OK, digits identical, segv 0 | 13.10 s |
| t_dbig new defaults | `./tests/t_dbig 22 0 29` | VERIFY OK (565 checks) | - |
| t_dbig old path | same, `DBIG_ADDSUB3=0 DBIG_MODQ_MONT=0` | VERIFY OK (565 checks) | - |

e9 (1e9 digits) identical in both bases = the three gates above (reference `~/ref/e_1000000000.txt`).
Skipped: the 4e10 / 1e11 step (no 4e10 reference on aac7; t01 restores references); `t_newton`/`t_mul`/mnaccept not run (kernels only; not requested).

aac6 cross-check (PPAC aac6-spx-a, lock s24-16, bundle clone `~/ntt-t08`, removed after): `tools/gate.sh --build --nodes 1 --hold J` PASS, VERIFY OK, digits identical, segv 0, ecalc total 11.42 s (measured, one run).

#### Notes
- gate.sh left `sleep 1500` orphans on aac7; the three of ours were killed by PID.
- First aac7 attempt used a wrong partition name (sbatch refused, nothing ran); fixed. aac6 node must be named `aac6-spx-a` for `-w`.

#### RESUME
Merge `t08` after the coordinator's 1- and 2-node gate; then update TASKS.md row 13 (K2+K3 adopted) and add a RESULTS.md line.

## ADOPT2 - `P24_CRT_SCAN=1` (D24) and `NEWTON_CHAIN_GUARD=1` (D25) as code defaults; two helper merges

(former file: ADOPT2.md)

**Change.** Both switches default to 1 (`=0` stays the fallback). `p24_crt_scan_on()` (p24_crt.c) and `newton_chain_guard()`
(newton_db.c) are the readers; the startup `config:` line shows `p24_crt_scan:%d,chain_guard:%d`. `mem_model.py` adds the guard's
+1 limb to the last doubling's start (`CHAIN_GUARD_CODE`, read from `NEWTON_CHAIN_GUARD`, as `binsplit.c dm_layout`).
Helper merges (SH3 follow-ups): `dmod128`/`dmod192` once in `rns_int.h` (rns_mul.c, p24_crt.c use it; was a copy), `db_powmod_h` once in
`dbig.c` (declared in `dbig.h`; `newton_db.c` had `mn_powmod`). Same behaviour.

**Checks (aac7, 2-node hold, SHMEM build; all measured).**
- `tools/gate.sh --build --nodes 2` PASS (VERIFY OK, digits identical, P24 in use, ecalc total 20.66 s); `--nodes 1` PASS (9.64 s).
  The `config:` line shows `p24_crt_scan:1,chain_guard:1`.
- `tests/t_newton 20` VERIFY OK (2756 checks); `mnaccept.sh --only unit` 9 passed (1556 s, t_mn_grid included, no hang: the unit step
  alone takes ~1500 s); `--only e9,mn` 6 passed (108 s), digits identical.
- `MN_PLAN_ONLY=38200000000000:576` runs (rc 0). `mem_model.py --check-c` exact (0 terms not exact) against `BS_LAYOUT_ONLY` at 1e10, 1e11 (1 node),
  6.441e10 x 2, and the 576-node share, with the guard 0 and 1.
- ABBA, 1 node, 1e10 digits, no digit output, A = guard 0, B = guard 1, 4 rounds (8 pairs of runs): B - A = +0.25 s,
  95% CI [-1.00, +1.50] s (n.s.; runs 15.9-19.4 s): no measurable cost.
- Not re-measured here: `P24_CRT_SCAN` has no effect at 1 node (multi-node only); its timing is results/K4c.md.

#### RESUME
Done; merged. Raw logs on the cluster under `~/adopt2/` (removed by the coordinator when no longer needed).

## DEF — P24 as the multi-node default (D18) and DM_MN_WRAP in the memory model + 6-node A/B (D19 b)

(former file: DEF.md)

Labels: **m** measured, **mod** modelled, **a** assumed. Times Eastern. Branch `def` (from `main` d1d19bf).

**Question.** D18: make the four-prime 24-digit product form P24 the default for every multi-node product, keeping the
three-prime multi-node path selectable. D19 (b): count `DM_MN_WRAP`'s extra block-pool memory in `dm_layout` /
`mem_model.py`, and time `DM_MN_WRAP=0` against `=1` at 6 nodes with the new default.

**Change / branch.** Branch `def`.
- `crt.c ec_np_planes`: under `ECALC_NP=auto`, pool 0 holds four planes at every g > 1, so `mn_p24_of()` (`rns_dist.c`) is
  true for every multi-node product. New setting **`ECALC_NP_MN3=1`** (off by default; review row: fallback) keeps the rule
  before D18: four planes only where the run's largest group can exceed the switch-over (from 32 node-processes).
- `ecalc.c`: `ECALC_NP` unset becomes `auto` at size > 1 (still 3 at size 1), so a multi-node run without a launch line also
  takes P24.
- `tools/gate.sh`: `--p24 auto|on|off` (forced P24 through `ECALC_NP_AUTO_TERMS=1000000`) is replaced by
  `--mn-primes p24|3`: at `--nodes` > 1 the gate checks the run log for `plane pool 0 for 4 primes` (default) or, with `3`,
  adds `ECALC_NP_MN3=1` and checks `plane pool 0 for 3 primes`.
- `mem_model.py np_planes` follows (reads `ECALC_NP_MN3` from the environment, as the C does).
- D19 (i): `binsplit.c dm_wrap_bytes` / `mem_model.py wrap_bytes`: with `DM_MN_WRAP` set and the wrap used
  (`newton_db.c mn_wrap_plane`'s rule), the mn division's low-product moment gains 2 quarters of P + 4 limbs per APU
  (Q·B^(−h) folded, held over both steps, and the folded A′ beside the assembly's Rc + Tt, where Aw + xql were). The
  `layout:` line prints ` wrap <bytes>` (absent when off: unchanged). `rns_dist.c`: `rns_mul_dist_mn_logcap_g`,
  `rns_mul_dist_mn_logmin_g` (by group size, for the layout).
- `DM_MN_WRAP`'s default is unchanged (0).

#### 1. Planner and memory model (login node, m)

Build `def` (SHMEM), aac7 launch line (`tools/profile_load.py aac7 --line`), output `~/def/login/` on aac7.

| check | result |
|---|---|
| `MN_PLAN_ONLY=3.82e13:576` (default) | `plan check … OK -- 1236 products`; `plan primes`: tree 42 of 42, recip 49 of 49, div 18 of 18 pieces at four primes (P24) |
| the same with `ECALC_NP_MN3=1` | identical output (only the timestamp line differs): at 576 the old rule already gave four planes |
| the same with `DM_MN_WRAP=1` | plan check OK; the layout's node 380.84 → 406.61 GB (+25.77 GB), with v-slots 388.51 → 414.28 GB |
| `BS_LAYOUT_ONLY` at 6.441e10 × {1, 2, 4, 6} and 3.82e13/576 × 576, four variants (default, `ECALC_NP_MN3=1`, `DM_MN_WRAP=1`, both) | `mem_model.py --check-c FILE 31 2048`: **0 terms not exact, largest arena difference 0.0000 %** in all four (20 plane figures each) |
| pool 0 at 6.441e10 per node, cap 2^31 (`planes:` line) | g 2, 4, 6: planes 120.88 GB per node (P24 default) against 103.70 with `ECALC_NP_MN3=1` (+17.18 GB per node, +4.3 GB per APU); g 576: 120.88 both |

`DM_MN_WRAP=1`'s term per APU (`wrap / dev`, C = model, mod): 8.59 GB at 2 and 4 nodes, 5.73 GB at 6, 7.64 GB at
3.82e13 on 576. At 3.82e13 on 576 the arena grows 214.75 → 240.52 GB per node and the room's node 386.84 → 412.61 GB:
**fits** the 480 GB budget (`room: … fits 1`, aac7 launch line; the target's own line is private and was not used).
At 2 nodes the model's dm need grows 8.6 GB per APU against the 4.3 GB per APU of pool growth measured in P1 (3-prime
form): the term is an upper bound (the assembly moment has no product scratch beside it, which the model's `div` keeps).

#### 2. Gates (aac7 queue, m)

`tools/gate.sh` at 10⁹ digits, aac7 launch line, digits compared with the reference. Items `84_def_gates2` (build `def`
ee85f5e, 20:15 ET), `85_def_gate4` (exclusive, build 5c84efb on the login node) and `86_def_regate` (after the rebase on
main 89318ae: `--build`, 5c84efb, 20:19 ET). Outputs `~/def/{gates,gate4,gates3}/` on aac7.

| gate | command | result | log check | device per APU, max (driver) |
|---|---|---|---|---|
| p24n2 (ee85f5e) | `--build --nodes 2` | PASS: rc 0, VERIFY OK, digits identical, 18.79 s | `plane pool 0 for 4 primes` (P24 in use) | 33.1 of 137.4 GB |
| p24n1 (ee85f5e) | `--nodes 1` | PASS, 12.27 s | `plane pool 0 for 3 primes` (size 1, unchanged) | 30.3 GB |
| mn3n2 (ee85f5e) | `--nodes 2 --mn-primes 3` (`ECALC_NP_MN3=1`) | PASS, 17.09 s | `plane pool 0 for 3 primes` | 28.8 GB |
| p24n4 (5c84efb) | `--nodes 4` | PASS, 20.30 s | `plane pool 0 for 4 primes` | 33.1 GB (in use 32.2: planes 120.3 GB per node + block pool 8.6) |
| p24n2 (5c84efb, rebased) | `--build --nodes 2` | PASS, 20.96 s | P24 in use | 33.1 GB |
| mn3n2 (5c84efb, rebased) | `--nodes 2 --mn-primes 3` | PASS, 17.03 s | three-prime path | 28.8 GB |

Memory headroom at 4 nodes: at 10⁹ digits 33.1 of 137.4 GB per APU (m). At the standard 6.441 × 10¹⁰ digits per node the
model gives the 4-node node peak 423.3 GB with P24 (`mem_per_node`, `ECALC_NP=auto`, shmem; mod), inside 480 GB;
MEAS-P24 measured 88.1 GB per APU in use at init at 2 nodes (m).

#### 3. 6-node A/B of DM_MN_WRAP with the P24 default (m)

Method (TESTING.md §6): queue item `87_def_abba6` (6 nodes, exclusive, user-approved D19 b), driver `~/def/def_abba.sh`,
2026-10-12 20:55–21:50 ET. Build `main` f232085 (P24 default, SHMEM) in `~/ntt-wt/defm`; 6 × 6.441 × 10¹⁰ = 3.8646 × 10¹¹
digits, no digit output; the aac7 launch line (`tools/profile_load.py aac7 --line`) + `COMM_XSTATS=1 MN_WAIT_STATS=1`;
A = `DM_MN_WRAP=0`, B = `DM_MN_WRAP=1`; rounds A B B A, cap 4 rounds, 60 min budget. All 16 runs rc 0 and VERIFY OK
(7 lines per log); every log `plane pool 0 for 4 primes`; B logs `DM_MN_WRAP: X Q from the low 12880065414 limbs + one
cyclic plane of 2^33 (w 21470000006)`. Runs took 181–229 s of `total`.

| quantity | A mean (8 runs) | B mean (8 runs) | B − A per round | **B − A mean, 95 % CI, 4 rounds** |
|---|---|---|---|---|
| `total` (s) | 199.40 | 196.54 | −4.13, −2.62, −6.89, +2.19 | **−2.86 [−8.92, +3.19], p 0.23** (round cap reached, stop rule not met) |
| division phase dm (s, `RESULT ecalc dm s`) | 84.88 | 78.63 | −3.46, −8.92, −9.26, −3.36 | **−6.25 [−11.47, −1.03], p 0.032** |

Device memory per APU (m, `mem[…] [init]` and `live:` lines, all runs alike per arm):

| | A (`DM_MN_WRAP=0`) | B (`=1`) | B − A |
|---|---|---|---|
| in use at init (planes 30.1 + arena + block pool) | 81.6 GB | 88.1 GB | +6.5 (the arena request now counts the wrap: `dm_layout`) |
| block pool at the division | 51.54 GB | 57.98 GB (60.13 on some APUs) | +6.44 (+8.59) |
| the division's window peak (step 2 X_lo Q) | 46.88 GB | 52.25 GB | **+5.37** (model's wrap term 5.73 GB, mod) |
| driver-reported peak used (of 137.4 GB) | 84.4 GB | 92.9 GB | +8.5 |

#### Verdict

Needs the user's decision; the switch stays off. At 6 nodes with the P24 default `DM_MN_WRAP=1` saves **6.25 s of the
division (95 % CI 1.0–11.5 s, p 0.032, 4 rounds, m)** but the `total` difference, −2.86 s [−8.92, +3.19], is not
significant after the 4-round cap (bs and init vary ±15 s between runs in both arms). It costs +5.4 GB per APU at the
division's peak (+6.4 to +8.6 GB of block pool, m), which the memory model now counts (5.73 GB per APU, mod; at 3.82 × 10¹³
on 576 the node with the room 386.8 → 412.6 GB, fits 480 GB, mod/m layout). D18 (P24 the multi-node default) is merged.

#### Open issues

- `BS_LAYOUT_ONLY` reads `ECALC_NP` once per process: lay out multi-node points with `ECALC_NP=auto` (as the launch line
  does); with it unset a multi-node layout point shows three planes although the run would take four.
- P24 at a non-power-of-two group below 32 nodes (e.g. 3 nodes in `mnaccept.sh`) is not gated here.

- The 6-node `total` difference is not significant after 4 rounds; a further 4 rounds (another 6-node hour) could settle it.
- Measured at 6 nodes only with P24; the model's wrap term was checked against one measured peak (6 nodes: 5.37 vs 5.73 GB
  per APU) and P1's 2-node pool growth (4.3 GB per APU against 8.59 modelled, 3-prime form).

#### RESUME

Done: merged to `main` (D18 default on; `DM_MN_WRAP` off), worktrees removed. Raw outputs on aac7: `~/def/login/` (plans,
layouts, `--check-c`), `~/def/{gates,gate4,gates3}/` (gates), `~/def/abba/` (`totals.tsv`: round, arm, seq, total, ok, dm,
rc, VERIFY lines, driver peak GB, pool GB, window peak GB; `stats.txt`, `log/`; driver `~/def/def_abba.sh <budget-min>`).
Next (user's call): adopt `DM_MN_WRAP=1` or not; optionally 4 more 6-node rounds for `total`.

## FIT — the memory fit before every launch, and DM_MN_WRAP automatic

(former file: FIT.md)

**Question.** User decision D26 (2026-10-13): "make it automatic, but we should only be scheduling jobs with efficient memory
allocations to begin with. So we need to know before we launch how well the memory fits." The planner's check (`MN_PLAN_ONLY`)
looked only at the 480 GB node budget, not at the device edge: at 3.82 × 10¹³ digits on 576 nodes, with `DM_MN_WRAP=1` (the default
since D22), the model put the device at 375.1 GB per node against the 373.44 GB edge measured on the target, and the plan still
said OK (results/NIGHT_RUNS.md §NIGHT, section 7).

**Change / branch.** Branch `fit`. No new algorithm; digits are unchanged (the wrap on or off gives the same digits, gates of D22).

- `binsplit.c binsplit_fit` (C, the source of truth): for (digits, nodes, settings) at rank 0's layout, the device peak per node
  = max(dm phase: planes + arena + comm_ofi pools + 4 v-slots; bs phase: the same + the VMM pool's bs growth 6.0 GB + the tree
  top's v-slots), per APU = / 4, against `ECALC_DEVICE_GB`; the node = planes + arena + bs growth + host + 4 v-slots against
  `ECALC_NODE_GB`; both margins; the terms. The comm_ofi pools count as device memory (fine-grained device memory by default;
  `COMM_OFI_POOL=host` moves them out). Printed by `MN_PLAN_ONLY` (`plan fit` lines; a run that does not fit ends with
  `plan REFUSED ...: the memory does not fit`, exit 8), by every run (`memory fit:` lines on rank 0) and by `BS_LAYOUT_ONLY`
  (a `fit:` line per point).
- New switches: `ECALC_DEVICE_GB` (default 373.44: the target's edge, 93.36 GB per APU, m, results/TARGET.md §TGTBENCH2) and
  `ECALC_DEVICE_MARGIN_GB` (default 5.0: the tighter of CAP17's two bars). `profiles/aac7.conf`: `ECALC_DEVICE_GB=444.0`
  (m: aac7 SPX t_edge, job 12287, VMM 444.0 GB committed, hipMalloc about 448.0; archive:RESULTS_ARCHIVE_1-114.md, section 107;
  the lower, graceful one is taken), on the aac7 launch line; `profiles/target.conf.template`: 373.44.
- `DM_MN_WRAP` automatic: unset, the run takes the wrap only when the device with it is at most `ECALC_DEVICE_GB` −
  `ECALC_DEVICE_MARGIN_GB`, else 0. Set by hand (0 / 1 / k), the set value wins, with a warning in the fit report when it does not
  fit. Every rank decides alike (rank 0's layout). The config line shows `dm_mn_wrap:<v>(fit|set|default)`; size 1 does not use
  the wrap (`default`, 1 as before). `newton_db.c dm_wrap` reads `binsplit_dm_wrap()`.
- Launchers: `tools/plan_values.py --fit <digits> <nodes>` (one entry point: the report, exit 1 = does not fit);
  `ecalc/mnrun.sh` (a block of its own after the pool block: plans the command's ecalc, prints the `plan fit` lines, refuses
  with exit 4 before srun; `MNRUN_FIT=0` skips); `tools/gate.sh` (before the allocation / the run: `fit.txt`, a FAIL when it does
  not fit; `GATE_FIT=0` skips); `ecalc/target_kit.sh` plan stage (the fit verdict and the wrap line in the summary; the refusal
  is the plan's last line, as a prime-set refusal). `plan_values.py` assumes `COMM_OFI_PLAN_CXI=1` when neither `COMM_OFI` nor
  it is set (mnrun.sh finds cxi on aac7's compute nodes at every launch; a login node has none).
- `mem_model.py`: `--check-c` ports the decision and compares the `fit:` line (wrap, device, node, fits, the device with and
  without the wrap); `mem_per_node` takes the wrap automatically by the same rule on its own `dev_max` (so `estimate.py --target`
  shows the target with the wrap off). `binsplit.c region_need` keeps its last two results (pure function; 0.18 s per call at
  the target's share): the planner with the fit takes 0.73 s against 0.58 s before (m, aac7 login node, MN_PLAN_ONLY at
  3.82 × 10¹³ on 576).

**Method.** Planner on the aac7 login node (no device). Target: `tools/plan_values.py --fit --profile target 38200000000000 576`
with `profiles/target.conf` = the template with its site keys filled by aac7 stand-ins (modules, paths; not tracked, deleted
after) -- every memory-relevant key is the template's (`COMM_SHMEM_ROUND_MB=1024`, `MN_T_CHUNK_MB=2048`, `ECALC_NP=auto`,
`COMM_OFI=1`, `ECALC_DEVICE_GB=373.44`, `ECALC_NODE_GB` = 480, the code default). aac7: `--profile aac7` at n × 6.441 × 10¹⁰
digits on n = 1, 2, 4, 6 (the S48 sizes). `mem_model.py --check-c` on `BS_LAYOUT_ONLY` output of six points. Gates: one aac7
queue item (see Results).

**Results.**

Memory fit (the planner's `plan fit` verdicts; m = computed by the planner on the aac7 login node, the figures themselves modelled
by the C layout):

| run | profile | device per node (per APU) vs limit | device margin | node vs 480 GB | node margin | DM_MN_WRAP |
|---|---|---|---|---|---|---|
| 3.82 × 10¹³ on 576 | target | 358.96 (89.74) / 373.44 | +14.48 | 395.85 | +84.15 | 0 (fit): with it 384.73 > 368.44 |
| 3.82 × 10¹³ on 576, `DM_MN_WRAP=1` set | target | 384.73 / 373.44 | −11.29 | 421.62 | +58.38 | 1 (set): WARNING, REFUSED |
| 6.441 × 10¹⁰ on 1 | aac7 | 290.09 / 444.00 | +153.91 | 316.44 | +163.56 | not used at size 1 |
| 1.2882 × 10¹¹ on 2 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit): without 368.47 |
| 2.5764 × 10¹¹ on 4 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit) |
| 3.8646 × 10¹¹ on 6 | aac7 | 374.22 / 444.00 | +69.78 | 409.24 | +70.76 | 1 (fit): without 348.45 |

Target terms at 3.82 × 10¹³ on 576 (the peak is the bs phase; the dm phase 352.96 GB): planes 120.88 + arena 214.75 + comm_ofi
pools 9.66 + bs growth 6.00 + v-slots 7.67 GB; per APU the arena 53.69 GB (the dm need 51.73: reciprocal set 38.53, division
37.45; the tree 50.93), plane pools 30.22. The wrap's term there is 7.64 GB per APU (the arena 214.75 → 240.52 GB per node).
NIGHT's 375.1 / 345.0 GB were mem_model's `dev_max`, which leaves out the comm_ofi pools (9.66 GB per node).

`mem_model.py --check-c` on BS_LAYOUT_ONLY at 4e10:1, 1e11:1, 6.632e10:576, 6.441e10:6, 1e9:2, 5e10:4 with the launch line's
settings (`COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=2048`, `ECALC_DEVICE_GB=444`): 0 terms not exact, the wrap decision equal at
every point (m).

Gates (aac7 queue items `20_fit_gates.sh`, `21_fit_gates2.sh`, commit 54f62a1 for the gates; m):

| check | result |
|---|---|
| `tools/gate.sh --build --nodes 2` | PASS: rc 0, VERIFY OK, digits identical, P24 in use; fit FITS 137.61 / 444.00 GB; config `dm_mn_wrap:0(fit)` (1e9 on 2: the wrap is not used at this size) |
| `tools/gate.sh --nodes 1` | PASS: rc 0, VERIFY OK, digits identical; fit FITS 120.44 / 444.00 GB |
| 6 nodes, 3.8646 × 10¹¹ digits, aac7 line through `mnrun.sh` (no digit output) | rc 0, VERIFY OK on all 7 lines; `mnrun.sh: plan fit FITS ... DM_MN_WRAP=1 (auto)` before srun and `memory fit:` in the run; config `dm_mn_wrap:1(fit)`; ecalc total 238.5 s (one run, not a timing claim) |

The first gate attempt (`20_`) failed at doccheck before building: the untracked stand-in `profiles/target.conf` was present in the
aac7 worktree and changed the registry's profile column. The stand-in was moved out and the two gates were run again (`21_`).
After rebasing onto main (SHM, STAT merged meanwhile; their changes touch the COMM_OFI=0 pool and static-analysis fixes) the
branch was rebuilt with `make -B SHMEM_CRAY=1` (`-DCOMM_SHMEM`): the target and aac7 6-node fit figures are unchanged, and
`--check-c` on 4e10:1, 6.632e10:576, 6.441e10:6, 1e9:2 has 0 inexact terms at both device limits (m). The gates were not rerun after the rebase.

**Verdict.** Done as decided (D26): the fit is computed before every launch and a run that does not fit is refused; the wrap is
automatic. The target at 3.82 × 10¹³ on 576 nodes fits with the wrap off (+14.48 GB device margin, m planner).

**Open issues.**
- The fit's device is the layout's, not a measurement on the target: the bs growth (6.0 GB) is measured at size 1 on aac7, and
  the comm_ofi pools depend on `COMM_OFI_PLAN_CXI` being right (mnrun.sh asks a compute node; plan_values assumes 1).
- `mem_model.py --check-c` without `COMM_SHMEM_ROUND_MB=1024` differs from the C in the comm_ofi pools at 6 and 576 nodes
  (`room ofi pools` -11.6 % / -19.5 %); this predates FIT (the room rows show it) and carries into the fit rows. With the launch
  lines' `COMM_SHMEM_ROUND_MB=1024` every row is exact.
- `mem_model.py`'s own `dev_max` (used by `estimate.py`) does not count the comm_ofi pools as device memory (345.0 GB at the target
  with the wrap off against the C's 358.96 GB); the C is the source of truth.
- aac7 without its profile gets the target's 373.44 GB edge (the code default): stricter, never looser.

**RESUME.** Done: merged to main, pushed; aac7 `~/ntt-acc` updated; worktrees removed. Gate logs: `ssh <cluster> 'cat ~/fit/SUMMARY.txt ~/fit/SUMMARY1.txt'`
(access: see the private protocol).

## S47 - exchange settings re-confirm at 4 nodes (aac7, batch 2026-10-11, track T2)

(former file: S47.md)

Status: **finished 2026-10-11 08:56 EDT; verdict INCONCLUSIVE at 4 nodes** (10 + 9 complete pairs). Labels: measured / modelled / assumed.

#### Question
Two settings on the aac7 launch line (`ecalc/e16_headline.sh`, 4 nodes, 6.441e10 digits per node = 2.5764e11 total, no digit output):
1. `MN_T_CHUNK_MB` 1024 (A) vs 2048 (B; the aac7 line since 2026-10-08, S22 D2: 24 rounds, mean -42.3 s at 10 nodes).
2. X2 off (A: `COMM_LAYER_INTER2=0 COMM_LAYER_VSLOT_POOL=0`) vs on (B: `=1 =1`; the aac7 line since 2026-10-09, S31: -31.7 s at 10 nodes).
Everything else is the current aac7 line (parsed from `e16_headline.sh`; includes `COMM_LAYER_VSLOT_SHARE=1`, kept in both X2 arms).

#### Design
- Build: `tools/gate.sh --build --nodes 1 --hold <hold>` in aac7 worktree `~/ntt-wt/s47` (detached at main `daeb0f9`): SHMEM (`-DCOMM_SHMEM` in build.log, `libsma` linked), 1 node 1e9 gate PASS (VERIFY OK, digits identical, measured). 4-node 1e9 gates with each non-default arm word set (`COMM_LAYER_INTER2=0 COMM_LAYER_VSLOT_POOL=0`; `MN_T_CHUNK_MB=1024`): PASS, digits identical (measured).
- Hold: `sbatch -N4 --exclusive -J b1011B -x aac7-n01,aac7-n61 -t <to S>`: job **12471**.
- Driver `archive:drivers/s47/s47_driver.sh` (+ `s47_stats.py`), on the aac7 login node, `srun --jobid --overlap` through `mnrun.sh`; `tools/rundriver.sh` for the watchdog (crash regex, 900 s stall, timeout max(2x, +900 s)).
- Phase 1, ABBA: the two comparisons are interleaved round by round (i then ii); in round r odd the order is A B, even B A. Before each run: reference files evicted from the page cache on the 4 nodes (`dd iflag=nocache`), no leftover `ecalc`, MemAvailable >= 428 GB on every node (waits up to 300 s). Stop rule per comparison, checked after every round: at least 6 complete pairs and (95 % CI of the paired wall difference excludes 0, or CI half-width <= 3 s), or 10 pairs (cap). Time budget 9000 s (2.5 h) from the start; 3 consecutive failed runs stop phase 1. A pair counts only if both runs are rc 0 + VERIFY OK.
- Phase 2, queue `~/batch1011/jobB/queue/*.sh` as the batch slot rules specify (serial, filename order, `# EST_MIN`/`# OWNER`, skip if now + EST would cross W = 05:56 EDT before the handoff or D = 08:56 EDT, timeout 1.5 x EST + 5 min with kill of the item's PID tree, `<item>.rc` + `<item>.log` written next to it as `${item%.sh}.rc/.log`, and also `<item>.rc`). At W: `scancel` the hold, submit B2 `-J b1011w --dependency=singleton -t <to S>`, poll >= 60 s, continue on B2 until D; then stats + marker, `scancel` own holds by id.
- Dry runs (measured, logic only, stub runs and stub items): ABBA with both stop rules, queue with ok / failing / over-long / hanging items (timeout path killed the tree, rc 124), W handoff (stub), D end. Outputs in `~/s47dry/`.

#### Commands
```
# armed (aac7 login node):  HOLD=12471 OUT=$HOME/s47/run setsid nohup bash -lc "cd ~; bash ~/s47/drv/s47_driver.sh" > ~/s47/driver.out 2>&1 < /dev/null &
python3 ~/s47/drv/s47_stats.py ~/s47/run/ab.tsv        # paired statistics (mean B-A, 95 % CI) per comparison
```

#### Results (measured; aac7, 4 nodes, 6.441e10 digits/node, hold 12471 then B2 12496; raw: aac7 ~/s47/run/{ab.tsv,stats.txt,results.txt,queue.tsv})
Paired difference B - A (positive = B slower), 95 % CI:
| comparison | pairs | ecalc total | driver wall | B faster in |
|---|---|---|---|---|
| (i) MN_T_CHUNK_MB 2048 (B) vs 1024 (A) | 10 | **+6.98 s [-10.32, +24.28]** | +8.00 s [-8.86, +24.86] | 5/10 (total) |
| (ii) X2 on (B) vs off (A) | 9 | -2.68 s [-12.30, +6.93] | +0.00 s [-7.70, +7.70] | 5/9 (total) |
Both CIs include 0: no difference shown; neither setting is confirmed or refuted. Digits: all 77 completed runs (see ab.tsv) VERIFY OK.

##### Bimodal totals
Complete runs fall in two clusters: **~181-185 s ("low")** and **~207-214 s ("high")**, a 25-27 s gap (the same size as the known slow-host stall, S32/S34, but no host was identified for it here); the first rounds are slower still (round 1: 225 and 281 s). Counts by cluster (threshold 195 s, rc 0 runs):
| comparison | arm | low (~182) | high (~208+) |
|---|---|---|---|
| (i) chunk | A 1024 | 7 | 3 (r1 224.7, r2 231.0, r6 209.0) |
| (i) chunk | B 2048 | 6 | 5 (r1 280.7, r2 207.7, r3 213.2, r4 207.4, r10 207.7) |
| (ii) X2 | A off | 5 | 4 (r1 231.0, r5 208.7, r7 207.3, r8 208.2) |
| (ii) X2 | B on | 5 | 5 (r1 213.7, r2 201.9, r4 207.6, r5 209.3, r7 208.7) |
Both modes occur in every arm, so the mode is not set by the setting under test; it is run-to-run noise (cause unknown; a slow-host or placement effect is a hypothesis, not tested). A pair whose two runs fall in different modes shows +-26 s with no real effect, which is why the CI half-width stays ~17 s (i) and ~10 s (ii) after 10 pairs; the stop rule (half-width <= 3 s) could not be met and the pair cap ended phase 1.

##### Failures
Two runs crashed with a segmentation fault at the end of init (before the first multiplication; 42 s each, rc 1, VERIFY FAILED), both arm A: **ii r2 A** (00:23 EDT, task 1 on aac7-n11) and **i r7 A** (01:19 EDT, task 0 on aac7-n10; G1 crash, init line printed `rns_init 16.69 s`). Their pairs were dropped (rounds 2 of (ii) and 7 of (i) are missing in the tables); the crashes are independent of the setting (arm A of different comparisons, different hosts) and look like the known intermittent init crash; not investigated here. ALERT file recorded the second.

##### Verdict and what would resolve it
Inconclusive at 4 nodes. The point estimates (+7.0 s for 2048, -2.7 s for X2 on) are well inside the noise (+-17 s, +-10 s). Both settings stay as they are on the aac7 line (their evidence is the 10-node S22/S31 runs). To resolve at 4 nodes: (a) find and remove the cause of the 26 s bimodality (log per-host stage times per run, run nodechk before each pair, or fix the node set with `-w` for a whole ABBA block); or (b) about 4x more pairs: the half-width scales ~1/sqrt(n), so reaching +-3 s from +-17 s needs roughly 30+ pairs for (i) (about 30 x 7 min = 3.5 h per comparison, modelled); or (c) a paired design that runs A and B back to back on the same nodes in the same allocation and uses the stage-level (not total) times, which should be less affected by the mode.

##### Queue (phase 2) and the OUT bug
13 queue items ran, all rc 0 (T3 10_1..10_4 02:18-02:33; T5 20_1..20_7 02:33-05:29; T4 30_nodechk 05:29; T5 21_s51_abba4b 07:27-08:53 EDT after the W handoff to B2).
**Queue bug (fixed in archive:drivers/s47/s47_driver.sh):** the driver was launched as `HOLD=.. OUT=$HOME/s47/run setsid nohup ...`, so OUT was an exported environment variable and every queue item inherited it; items that use their own `OUT` (e.g. as an output directory) would have written into the driver's directory. Fix: `run_item` now starts the item with `env -u OUT -u HOLD`. Alternative for the launch line: set the variable without exporting. Not re-run live (logic unchanged otherwise).

#### RESUME
- Driver output dir (aac7): `~/s47/run/` (summary.txt = timeline, ab.tsv = all runs, stats.txt, results.txt = digest, ALERT, queue.tsv, log/). Driver stdout `~/s47/driver.out`. The driver copies the binary to `~/s47/run/ecalc_s47` (md5 in summary.txt).
- Markers: phase 1 `~/s47/run/S47_ABBA_DONE`; whole driver `~/s47/run/S47_DONE` (first line SUCCESS / FAILED).
- Collect: `ssh aac7 'cat ~/s47/run/S47_ABBA_DONE; cat ~/s47/run/stats.txt; cat ~/s47/run/queue.tsv; head -30 ~/s47/run/ALERT 2>/dev/null'` then copy `stats.txt` into this file and into RESULTS.md (coordinator).
- Cleanup after D: remove aac7 worktree `git -C ~/ntt-acc worktree remove --force ~/ntt-wt/s47`; holds are cancelled by the driver (check `squeue -u <user>` for 12471 and any b1011w job of this driver).
- Interpretation rule (user's): adopt/keep only on a CI that excludes 0 with identical digits; T2048 and X2 are already on the aac7 line, so this is a re-confirmation at 4 nodes.
