# EST17 — re-estimate of the 576-node run with `comm_ofi` now the default (2026-10-06)

No cluster jobs run for this note: all figures are `ecalc/estimate.py` / `ecalc/mn_model.py` arithmetic, local, on the repo
at `main` (post-rebase: `8e03ba0`, STD17). Inputs and labels follow results/OFI17.md (the aac7 `comm_ofi` measurements),
internal code notes (the switch), internal code notes §5 (the standing-estimate method this note
updates) and results/STD17.md (the OFI memory accounting, §108).

## 1. Why the standing estimate moves

The standing figure at internal target notes §1 (`290.2 / 303.9 s` at ROCm 7.2.4) assumed one SHMEM transport on one NIC per
APU at **100 GB/s per APU, line rate** (two assumed 400 Gb/s NICs at 100 % efficiency — never measured). `comm_ofi`
(Phase 17, now the default wherever a cxi NIC is present) is **measured** on aac7 instead: it stripes a node's exchange
over every NIC of the calling APU's device and holds a flat aggregate from 4 nodes on, far above SHMEM's one-NIC cap, but
still well under line rate per NIC:

| nodes | SHMEM aggregate/node (measured) | `comm_ofi` aggregate/node (measured) | per-NIC microbench (measured, NIC16) |
|---|---|---|---|
| 2 | 19.8 GB/s | 25.0 GB/s | 23.3 GB/s |
| 4 | 14.9 GB/s | 48.5 GB/s | |
| 8 | 11.7 GB/s | 44.4 GB/s | |
| 10 | 12.3 GB/s | 44.1 GB/s | |

aac7 has **4 NICs per node, one per APU, 200 Gb/s (25 GB/s) each**. `comm_ofi`'s aggregate/4 NICs ≈ 11–12 GB/s per NIC
from 8 nodes on — **measured efficiency ≈ 0.47–0.5 of the 23.3 GB/s per-NIC line rate** (NIC16), not of the 25 GB/s wire
rate. The 4→10 node range is flat (44.4 → 44.1 GB/s), so this note's main rows carry **no fall-off** with node count
(§4 gives the sensitivity to a fall-off anyway, since the target's NIC count, topology and the dragonfly's global links
are unmeasured at 576).

Four `--bw` (GB/s per APU injection) cases follow, all **modelled** given the labelled input:

| case | per-APU rate | how it is built | label |
|---|---|---|---|
| (i) 4 NICs/node (today's aac7 hardware) | **11 GB/s** | the measured `comm_ofi` aggregate/node ÷ 4 NICs at 8–10 nodes (11.1–11.0 GB/s) | measured (aac7) |
| (ii) 8 NICs/node (the target's hardware, PLAN §25) | **47 GB/s** | 2 × 400 Gb/s NICs/APU × 50 GB/s line rate × 0.47 measured efficiency | assumed (efficiency carried from aac7; NIC count and 400 Gb/s from PLAN) |
| (iii) line rate (the old assumption) | **100 GB/s** | 2 × 400 Gb/s NICs/APU at 100 % efficiency | assumed (unchanged from internal target notes §1) |
| (iv) old SHMEM, 1 NIC/node (for contrast) | **3.6 GB/s** | `mn_model.AAC7` profile, fitted on the 2-node SHMEM measurement (C16) | measured (aac7, pre-`comm_ofi`) |

`comm_ofi` also removes the transport's device-staging D2H/H2D copy that an earlier model term (`--staging-copy`) priced
for a hypothetical staged transport: no transport on the target stages exchanges through host memory today (under
`comm_ofi` the device pools are registered and written to directly), so every row below uses the model's default
(`--staging-copy` omitted, `staging_bw=None`, off) — unchanged from before `comm_ofi` existed.

## 2. Command

```
DM_MN_LEAN=1 MN_OUT_DKM_HI=1 MN_MODEL_MAP_RATE=0.070 \
  ./estimate.py --target --lat 8.5e-6 --hide-pow2 0.72 --t-round 0.015 --bw <BW> [--local-factor 1.22] [--fall-off <A>]
```

`DM_MN_LEAN=1` (int15k, 2026-10-05: −20 GB/node modelled, node 471.9 → 446.2 GB, time unchanged) and `MN_OUT_DKM_HI=1`
(the launch line's partial-cache row, `estimate.py`'s `partial_row`) are both on the launch line, not code defaults.
`--lat 8.5e-6`, `--hide-pow2 0.72`, `--t-round 0.015`, `MN_MODEL_MAP_RATE=0.070` are all aac7-measured (internal code notes
§3.1 Profiles, `TARGET_TASKS.md` T1). `--local-factor 1.22` prices the ROCm 7.0.3 toolchain (measured +22 % on bs/dm at
10¹¹ on aac7, C16); its absence is the 7.2.4 figure (aac6's calibration, factor 1.0). The table below reads the
**launch line's `RNS_DIST_CACHE_PARTIAL=1` row** (`partial_row`, the headline figure internal target notes §1 reports), "with
write" = the write column at 1.0 GB/s/node (576 writers, assumed to hold from one node measured, internal code notes Profiles).

Memory note: `estimate.py` run locally (no cxi NIC on this host) models `COMM_OFI` memory accounting (results/STD17.md
§108) only if told to — `mem_model.ofi_planned()` defaults off without a cxi device or `COMM_OFI`/`COMM_OFI_PLAN_CXI` set
in the environment. Node GB below is given both ways: `COMM_OFI` unset (off on this host, matching the launch-line
figure before STD17's accounting) and `COMM_OFI=1` (modelling the target's Slingshot nodes, which do have cxi). Time is
unaffected either way — STD17's accounting is a memory-only correction.

## 3. Walls at the target and the second test size (modelled; write @ 1.0 GB/s/node)

| case (`--bw`) | ROCm 7.2.4 no-write / write | ROCm 7.0.3 (`--local-factor 1.22`) no-write / write |
|---|---|---|
| **5.276 × 10¹³ digits (the target)** | | |
| (i) 4 NICs, measured eff. (bw 11) | 750.1 / 744.6 s | 795.6 / 790.0 s |
| (ii) 8 NICs, same eff. (bw 47, assumed) | 352.6 / 360.6 s | 398.1 / 403.8 s |
| (iii) line rate (bw 100, assumed, old) | 288.0 / 302.7 s | 333.8 / 346.1 s |
| (iv) old SHMEM 1 NIC (bw 3.6, measured) | 1816.1 / 1810.5 s | 1861.6 / 1856.0 s |
| **5.167 × 10¹³ digits (the second test size)** | | |
| (i) 4 NICs, measured eff. (bw 11) | 734.0 / 728.6 s | 778.6 / 773.1 s |
| (ii) 8 NICs, same eff. (bw 47, assumed) | 345.1 / 352.4 s | 389.6 / 394.6 s |
| (iii) line rate (bw 100, assumed, old) | 281.8 / 295.8 s | 326.6 / 338.2 s |
| (iv) old SHMEM 1 NIC (bw 3.6, measured) | 1777.2 / 1771.7 s | 1821.7 / 1816.2 s |

Node memory at the target (5.276 × 10¹³; unaffected by `--bw`, `--local-factor` or the fall-off below): **446.2 GB**
(device 403.2 + host 44.4; SHMEM pool 9.9) with `COMM_OFI` unset on this host; **447.5 GB** (403.2 + 45.7; pool 11.3,
of which the per-device comm pools are 4 × ≈0.25 GB) modelling the target's cxi nodes (`COMM_OFI=1`; results/STD17.md
§108's accounting) — a **+1.3 GB** correction, well inside the 480 GB budget either way.

## 4. `--fall-off` sensitivity (a = 0 and a = 0.3; cases (i)–(iii), 7.2.4, write @ 1.0 GB/s/node)

`comm_ofi`'s own 4→10 node aac7 data is flat (44.4 → 44.1 GB/s, §1) — **a = 0 is this note's main-row assumption**
(the rows in §3 above). `a = 0.3` is the sensitivity the model already carries for an unmeasured fall-off at 576 nodes
(`mn_model.Fabric.fall_off`, rate × (g/2)^(−a); no aac7 t_comm fall-off measurement at 576 is fed in by default):

| case | a = 0 (§3 above) | a = 0.3 | Δ |
|---|---|---|---|
| (i) bw 11 | 750.1 / 744.6 s | 2134.6 / 2129.0 s | ×2.8–2.9 |
| (ii) bw 47 | 352.6 / 360.6 s | 676.9 / 671.3 s | ×1.9 |
| (iii) bw 100 | 288.0 / 302.7 s | 440.6 / 435.0 s | ×1.4–1.5 |

(second test size, 5.167 × 10¹³, same a = 0.3: (i) 2072.6 / 2067.1 s, (ii) 658.5 / 653.1 s, (iii) 429.3 / 423.9 s.)

A fall-off this size would matter more than the NIC/efficiency choice between (i) and (ii) — it is the single largest
lever in this table, consistent with internal code notes §5's reading that injection bandwidth (and, by the same mechanism,
its fall-off with scale) is the only input that can move the answer by more than ±20%.

## 5. Reading

- **comm_ofi's measured 4-NIC aac7 efficiency (bw 11, case i) gives 750/745 s at the target** — 2.5–2.6× the old
  100 GB/s-line-rate figure (288/303 s) and 2.1× the 8-NIC/same-efficiency case (ii, 353/361 s). The gap between (i)
  and (iii) is almost entirely the efficiency factor (0.47 vs 1.0) and the NIC count (4 vs 8 assumed on the target),
  not a new mechanism.
- **Case (ii) (8 NICs, the target's hardware, at aac7's measured per-NIC efficiency) is the most defensible number for
  planning**: 353/361 s at 7.2.4, 398/404 s at 7.0.3 — both still well above the old 288/303 s standing estimate, because
  that estimate assumed line-rate injection nobody has measured.
- **comm_ofi is still a large win over the old SHMEM 1-NIC form**: case (iv) at 1816/1811 s is 2.4× case (i) and 6.3×
  case (ii) — `comm_ofi`'s multi-NIC striping is the dominant lever available today, even before the target's NIC count
  or line rate are known.
- **The fall-off is the open risk, not the NIC/efficiency choice**: at a = 0.3 every case roughly doubles (case iii) to
  triples (case i); `comm_ofi`'s own aac7 data argues against it (flat 4→10 nodes) but no measurement exists at the
  scale (64, 192, 576 nodes) or topology (dragonfly, multiple groups) of the target.
- Memory is a non-issue either way: +1.3 GB from counting the OFI comm pools correctly (§3) is noise against the
  446–448 GB range, itself well inside the 480 GB budget.

## 6. Labels

measured: the comm_ofi/SHMEM aac7 aggregate and per-NIC tables (§1), `--lat`/`--hide-pow2`/`--t-round`/`MN_MODEL_MAP_RATE`
(aac7, internal code notes Profiles), the 7.0.3 factor 1.22 (aac7, 10¹¹), bw 3.6 (aac7 SHMEM profile fit), the OFI pool
accounting delta (STD17 §108, exact against the C layout there).
modelled: every wall and node-GB figure (`estimate.py`/`mn_model.py`/`mem_model.py` arithmetic on the measured/assumed
inputs below), the fall-off sensitivity's a = 0.3 multiplier (the model's own term, not a measurement at scale).
assumed: bw 47 (8-NIC extrapolation of aac7's measured efficiency; the NIC count and 400 Gb/s line rate are PLAN §25's,
unmeasured on the target), bw 100 (the old line-rate assumption, unchanged, now superseded as a lower bound by the
efficiency correction), the fall-off exponent a = 0.3 itself, 576 writers at 1.0 GB/s/node holding from one node
measured, dragonfly group/taper/layers unchanged from internal target notes §1.
