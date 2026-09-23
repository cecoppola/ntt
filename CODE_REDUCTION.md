# Code reduction: a recommendation for every opportunity

State: `ecalc/` is 13 016 lines of core C/HIP (26 units, 19 headers), 3 052 lines of tests,
1 030 lines of Python models, 116 environment switches. Every item below was checked
against the source — reachability, call sites, and whether two similar things share a
core already — rather than judged by name.

Categories, as requested:

- **(a) archive** — useful test or diagnostic code: moved to `archive/`, not deleted.
- **(b) remove** — vestigial, superseded, no measurement left to make.
- **(c) merge** — essentially duplicated; one implementation should serve both.
- **(d) retain** — similar-looking but distinct for performance or complexity reasons.
- **(e) other** — retained for a reason that is none of the above (usually verification value).

Totals: **(a) ≈ 320 lines archived, (b) ≈ 1 400 removed, (c) ≈ 130 saved by merging**
→ about **1 850 lines, 14 %**, leaving ≈ 11 170 with performance unchanged and 56 of the
116 switches gone.

---

## (a) Archive — instruments that did their job

These found real defects and may be needed again on new hardware or a new ROCm. They cost
nothing at run time but they are read every time someone touches the file they live in.
Move the code to `archive/instruments/` with a manifest entry naming what each found.

| item | lines | what it found | recommendation |
|---|---|---|---|
| `ECALC_RES_LOG`, `_LEVEL`, `_CPU` — every residue cross-checked (kernel vs host Horner, recurrence vs main thread, per-node shares, per-level leaves) | ~120 | the composite check moduli; localized the D5 failure to a leaf level | archive; **keep** the `--stress` regression step, which is the permanent guard |
| `ECALC_LEAF_DUMP` — the first wrong node and its children as raw limbs | ~40 | the leaf-level signature of D5 | archive |
| `ECALC_COPY_PROBE` — times the odd-node copy against the next level | ~35 | 47 of 52 copies completing after the next kernel launched | archive; **keep `tests/t_copy_order.c`** (97 lines), which reproduces the ordering standalone |
| `ECALC_B_SNAPSHOT` — each device snapshots the shared operand before reading it | ~30 | confirmed the stale operand, not a wrong number | archive |
| `MEM_DPOOL_FILL`, `MEM_COPY_NOWAIT` | ~25 | fill a grown pool with 0xA5; reproduce the pre-fix copy | archive (`t_copy_order` covers the second) |
| `DBIG_SERIAL`, `DBIG_WARM`, `MEM_NO_DEV_MEMSET` | ~30 | quarter-by-quarter debugging; page-warming experiments | archive |
| trace switches `MN_LAYERED_RAW`, `MN_LAYERED_LOCAL`, `MN_DEBUG`, `MN_TOPO_TRACE`, `COMM_SHMEM_TRACE` | ~40 | communicator bring-up | archive |

**Keep in `tests/` permanently** (not archived): `t_alloc.c` (279) and `t_copy_order.c` (97).
These are the two measurements a port to new hardware must repeat first, and §3 and §10 of
the paper tell readers to run them.

---

## (b) Remove — superseded, with the measurement preserved in the record

| item | lines | why it can go |
|---|---|---|
| **Engine 2** — `ntt2.c` (509), `ntt2.h` (66), `modarith2.h` (65), `crt2.c` (57), 5 dispatch sites in `rns_mul.c` (~60) | **~757** | two 62-bit primes with 45-bit points, measured end-to-end in Phase 5 and rejected. The four-prime FP64 engine is now structural: the coefficient bound, the plane sizes and the exactness rules of the paper's §2.5 all rest on it. There is no configuration in which engine 2 would be chosen, and re-measuring it would mean re-deriving the design. RESULTS §44 keeps the numbers. |
| **`newton.c`** — the host reciprocal and division | 231 | reachable only via `NEWTON_DEVICE=0`. `t_newton` uses it as a GMP-checked oracle, but every test in this suite needs a GPU anyway, so the oracle can be `newton_db` directly. Archive the file; delete the switch. |
| host-flow stand-ins `MN_DM=host`, `MN_COMBINE=host` | ~40 | the scaffolding by which the multi-node run first worked. The sharded division and per-node output replaced them, and the race they were kept to cross-check is closed with a named cause. |
| rejected layout and placement experiments: `RNS_CRT_LAYOUT=1`, `RNS_REPACK_WIDE`, `BS_REGION_FLAT`, `BS_REGION_SLACK`, `ECALC_POOL_GROW_GB`, `MEM_PIN`, `BI_HUGE`, `RNS_DIST_CACHE_HOLD`, `ECALC_STOP_AFTER_BS`, `COMM_SHMEM_FENCE`, `COMM_SHMEM_NOSYM` | ~200 | each measured worse, each now contradicted by a structural change (the simulated layout, the reserved tail, the pool-resident slabs) |
| `MEM_ALLOC`'s five losing forms | ~70 | `t_alloc` measures all six standalone; the pipeline needs only `hipMalloc`. Keep the *finding*, drop the code path. |
| `NTT_B16_XCHG` — the DPP/`ds_swizzle` transform body | ~55 | bit-identical and slower (1.43 → 1.23 TB/s); the LDS pipe was never the bottleneck |
| legacy flows: `ECALC_STAGING` modes 0/2, `BS_DEVICE_POOLS=0`, `BS_DEV_MDEV=0`, `RNS_BATCH_LOCAL=0`, `ECALC_OVERLAP=0` | ~50 | superseded defaults; the guards are thin because the paths largely share code, which is why this line is small |

**Total (b): ≈ 1 400 lines, 46 switches.**

One caution: removing `ECALC_OVERLAP=0` removes the ability to A/B the overlap in one
binary. The comparison is in RESULTS §68 and the sequential figure (128.8 s clean) is
recorded; if you would rather keep one flow switch for future scheduling work, keep this one.

---

## (c) Merge — essentially duplicated

Only two genuine cases survived inspection. Everything else that *looked* duplicated is in (d).

| item | lines saved | recommendation |
|---|---|---|
| **Piece-selection policy.** `split_grid_cap` (`rns_dist.c`) chooses a cost-minimizing $k_a \times k_b$ grid under a plane cap; `mul_karatsuba`/`mul_chunked` (`rns_mul.c`, 91 lines) cut an oversized product by halving or chunking. Two policies for one decision, and the grid rule is strictly better informed (it knows the cap, the $3\cdot2^k$ lengths and the per-point cost). | ~60 | extract the selection into one function taking (sizes, cap, r3) and returning the cut; let both tiers call it. The *execution* stays separate — one works on host bigints, the other on device numbers — so this merges the policy, not the machinery. Low risk; re-verify with `t_mul`. |
| **Grid execution.** `mul_grid` (single node, `dbig`) and `mn_grid` (sharded, `mdb`) implement the same loop: iterate pieces, apply the high/low cut predicates, multiply, accumulate with a shift. | ~70 | factor the piece iteration and the cut predicates into a shared helper parameterized by an operand accessor; keep the two accumulate paths, which differ genuinely (local add vs cross-node carry scan). Medium risk — this is live code at every size — so do it alone, with the full regression either side. |

**Do not merge** the reporting code scattered across files into one module: it reads as
noise but each line is adjacent to the quantity it prints, and the per-phase accounting is
what the paper's §9 tables are built from.

---

## (d) Retain — similar for a reason

Each of these looks like duplication and is not. I checked call sites and cores in every case.

| apparent duplication | why it stays |
|---|---|
| **Three big-integer layers**: `bi_*` (host), `db_*` (four device quarters), `mdb_*` (sharded over nodes) | Different memory topologies with different carry problems: a host loop, a per-APU kernel with a 4096-limb chunk scan, and a cross-node scan over node boundaries. They are *already* factored where they can be — `db_share_addsub` and `db_share_add_shifted` are thin wrappers over one `addsub_core2`, and `mdb_*` calls them. Unifying further would mean an abstraction over "where the limbs are", which is exactly the thing the design is organized around. |
| **Six product tiers**: schoolbook, one-prime-per-device, karatsuba/chunked, batch, batch-local/striped-pair, distributed (4 APUs and node group) | Dispatched by operand size, and each exists because a different resource binds at that size: CPU arithmetic below 1024 points, one APU's plane, several planes, four APUs, a node group. All are reached in the default path — none is dead. A single generic path would reintroduce the special cases inside itself, or lose the pairing and locality wins that §5 measures at 4.6 s and 10 s. |
| **`rns_mul_dist`, `_dist_db`, `_dist_hd`, `_dist_mn`** | four entry points over *one* `dist_core`, differing only in where operands live (host, device, mixed, sharded). Already factored. |
| **`rns_mul_batch_local` vs the striped/pair path** | two strategies for different level shapes: prime-per-device when the level's products fit one plane set, striped with pairing when they do not. The dispatch is three lines and the measurement that justifies it is RESULTS §74. |
| **Five communicator implementations** | genuinely different mechanisms, and each earns its place: `local` (size 1), `sim4` (four synthetic ranks — the only way to test the distributed transform with no fabric, on any machine), `xgmi` (the push kernel, intra-node), `tcp` (needs no OpenSHMEM install; the portable fallback and the whole development history), `shmem` (the target). 1 680 lines for the interface plus five transports is cheap for that coverage. |
| **`ntt3.c`** radix-3 lengths | adopted, not vestigial: it gives length granularity of 1.5× instead of 2×, which the grid rule actively exploits. |
| **single-node vs sharded reciprocal** (`newton_db_recip` / `recip_mn`) | different operand types and different cost models — the sharded one chooses a subgroup per doubling. Sharing more would mean threading the group through every single-node call. |

---

## (e) Other — retained for reasons outside performance

| item | lines | reason |
|---|---|---|
| **The binary pipeline**: `todec.c` (305) plus ~120 lines of `LIMB_BASE=2` branches | ~425 | Not kept for speed — it is slower. It is the only **independent pipeline**: the same digits by a different representation, with a different division path and a radix conversion the decimal pipeline does not have. The regression's two-base comparison at $10^9$ digits is the check that catches a representation-specific fault, and it is the single largest easy cut on the table. Recommend **keep**, and state the cost explicitly in the README so the trade is visible. |
| **The models** `mem_model.py`, `mn_model.py`, `estimate.py` | 1 030 | not in the binary; they are how a run is planned and how the 576-node figures are derived. One of them (`mem_model`) is a line-by-line port of the C arena sizing, so a divergence is a detectable bug. |
| **`mnaccept.sh` and the harnesses** | ~400 | the regression is the safety net every other reduction here depends on |
| **`results/*.md`** | — | the measurement record the papers cite; tracked by rule since Phase 12 |

---

## Sequence

1. **(a) archive** — mechanical, no behavior change. Regression green before and after.
2. **(b) remove** in two commits: engine 2 alone (it is 54 % of the removal and touches the
   dispatch in `rns_mul.c`), then everything else. Regression after each.
3. **(c) merge** the piece-selection policy; regression. Then, separately and only if you
   want it, the grid execution helper; full regression plus a $4\times10^{10}$ timing series
   either side, since that one is live code at every size.
4. Update `ecalc/README.md`'s switch list (it is grep-verified against the source, so it
   will fail loudly if a switch is removed and left listed).

After (a)–(c): **≈ 11 170 lines, 60 switches**, same performance, same capability, with
every removed path preserved under `archive/` and its measurement cited in `RESULTS.md`.

## The archive

```
archive/
  MANIFEST.md           one row per item: what it was, why removed, RESULTS section, commit
  engine2/              ntt2.c, ntt2.h, modarith2.h, crt2.c and the dispatch diff
  newton_host/          newton.c and its declarations
  instruments/          the probes of (a), each with the defect it found
  experiments/          the rejected layout and placement switches, as patches
```

Git history preserves all of it regardless; the folder exists so that a future reader can
find the losing branch without knowing which commit removed it.
