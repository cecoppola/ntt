# H — host memory: X never on the host (B1), the seeds streamed to the regions (B2) (Phase 10, PLAN.md §21)

Branch `h10` (from `main` @ 38ed61b, the Phase 9 merge). Owned: `ecalc/mn_out.c/.h`, `ecalc/verify.c/.h`
(untouched), the output tail of `ecalc/ecalc.c`, the seed code of `ecalc/binsplit.c`, the staging size in
`rns_init`'s request. Touched outside, all minimal and commented `Phase 10 H`: `ecalc/newton_db.c` + `newton.h`
(G: one hook variable and a 6-line helper), `ecalc/dbig.c` (a mutex around `db_mod_qs`), `ecalc/mem.c/.h`
(one additive async-copy pair), the `binsplit_pregrow` entry (M: a one-line once-per-run guard).

## B1 — X never on the host

**Size 1 (the device flow).** `newton_db_divmod_shifted` gains `newton_db_x_dev`: when set, X is moved there
(the device number, off the block pool) before the low product instead of being copied to the host
(`db_to_bi`, 17.8 GB at 4 × 10¹⁰, ≈ 0.5 s on the critical path); the ±1 corrections at the end are applied
to it in place (`db_share_add_val` over its n limbs; a carry out of the top limb aborts — it cannot happen
for e's X, whose top limb is 2…); the x hook is called as before, with the empty host X. The background
thread of the output stage (`x_bg`) then computes X's residues by the device kernel (`db_mod_qs`, 0.55 s
at 4 × 10¹⁰ instead of 2.8 s of host Horner) and runs the chunked writer over `src.dev` (A-out's device path,
until now used only by the multi-node shares): each 256 MB chunk's limbs come off the device by
`hipMemcpyAsync` on a non-blocking stream per APU into the pinned limb buffer (a null-stream `hipMemcpy`
would serialise with every kernel of the low product), formatted, residue-checked, windowed, written. The
block pool is released after the output stage instead of after the division (X lives in it). A correction
(0 in every run so far) is handled as before: the main thread joins the writer, recomputes X's residues from
the corrected device X and redoes the digits (the file is rewritten); the common path waits for nothing —
T1 waits only for the residue semaphore. `ECALC_OVERLAP=0` keeps the host flow (host A, host X; unchanged).

**Size > 1 (the distributed division).** `out_stage` reads this node's share of A-div's `mdb` X in place
(`src.dev = &Xm.sh`, `mdb_share` for `[lo, hi)`, `cnt` clipped to the share's length) — the stand-in
scatter from node 0's host X is gone from that flow (it stays for `MN_DM=host` / `MN_COMBINE=host`, where X
exists only on node 0's host). The residues of P, Q, R are every node's own: `newton_mn_divmod` already
computes them by `mdb_mod_qs` (each share's `db_mod_qs` scaled by B^lo and summed over the group — exactly
the "db_mod_qs on the share + mn_out_res_combine" of the hook comment), so the broadcast of node 0's is
dropped there; `out_ctx` receives them right after the sharded division, before the non-zero ranks leave
(the first multi-node run failed T1 on ranks ≥ 1 because the copy sat after their exit point). Node 0 no
longer gathers X (`mn_gather_host` of X is unused by this flow); the block pool is released after the output
stage on every rank. `mn_out.c`'s `db_mod_qs` shares a static scratch with the main thread's R residues, so
`db_mod_qs` now takes a mutex (dbig.c).

## B2 — the seeds streamed into the regions

Before: each region's spans were computed into a region-sized pinned staging on its APU (4 × 10.7 GB at
4 × 10¹⁰: the largest pinned item of the run and the host peak of the bs phase), then copied into the region
by one DMA per region after all of them (I2: computed in a background thread during init, copied at the
start of bs). Now (`seeds_stream` in binsplit.c):

* `binsplit_seeds_begin` — the `rns_after_staging_hook`, called inside `rns_init` once the staging exists —
  starts the seed thread and then allocates the region pools itself (`binsplit_pregrow`, now once per run:
  the driver's later call returns; the same 96 GB mapped 5 s earlier, before the plane pools), then posts
  "pools ready" to the thread (a mutex/condvar).
* The thread computes the spans in chunks of `BS_SEED_CHUNK_MB` (2048; 20 chunks at 4 × 10¹⁰) with all
  threads (`omp for schedule(dynamic, 16)`; the old region-pinned assignment only matters with `MEM_PIN`,
  which is off). While the regions do not exist yet, a chunk goes into one of two pinned buffers (2 GiB each,
  `mem_hstage_alloc`, freed after the seeds); once they exist the threads **store straight into the region**
  (`BS_SEED_DIRECT=1`, default): CPU stores into device memory at the rate of the schoolbook (the seeds are
  compute-bound; RESULTS §56 measured streaming stores at ≈ 8 GB/s against ≈ 5 GB/s of seeds), no HIP call
  from the seed thread. The buffered chunks (two at most: the thread waits for the regions when both are
  full) are copied by `hipMemcpyAsync` on a non-blocking stream (`mem_dev_copy_async`, new in mem.c) once the
  regions exist. `BS_SEED_DIRECT=0` sends every chunk through a buffer and a DMA (measured below: slower —
  a HIP call from the seed thread blocks behind the main thread's `hipMalloc` of the plane pools, 0.4 s per
  call; on the null stream it was worse, the copy queued behind the plane pools' `hipMemset` at 4 GB/s).
* `binsplit_e` joins the thread, takes the node table and checks that the region pools are the ones the
  seeds went into (abort otherwise). Without the overlap (`ECALC_OVERLAP=0`, no hook) the same function runs
  synchronously in `binsplit_e` (all chunks direct). Host regions (`BS_DEVICE_POOLS=0`) keep the old path
  with malloc'd staging.
* The pinned staging of `rns_init` is no longer used by the seeds: in the decimal device flow it is 1 GiB
  per APU (the checkpoints' chunk buffer; `ECALC_STAGING=2` restores the seed-sized staging, `=0` the paper's
  64 GiB). The batch tiers never touch it in this flow (all products are device-registered, `gpucrt` always).

## Tests (aac6, one node, clone `~/ntt-h`; jobs 20763, 20768, 20770, 20781, 20787; logs `~/h/`)

Every run VERIFY OK on every node and the digits byte-identical (`cmp`) to `~/ntt/ecalc/ref/e_<d>.txt`
(sizes > 1: `cat` of the sorted part files) or `results/e_4e10.out`. Size 1: `srun --jobid=$J -N1 --gpus=4
bash -lc "module load rocm; cd ~/ntt-h/ecalc; env <env> ./ecalc <d> /tmp/h_x.txt"`; size > 1:
`SLURM_JOB_ID=$J ./mnrun.sh <p> env <env> ./ecalc <d> /tmp/h_x.txt`. Scripts `~/h_batch{1,3,4,5}.sh`.

| run | env | result |
|---|---|---|
| t_out, t_newton, t_dbig 0 big, t_verify, t_bs | | OK (1, 696, 609, 334, 10 checks; t_bs's two SHA lines read `ref/*.sha256`, absent from the clone — the printed hashes equal `~/ntt/ecalc/ref`'s; `~/ntt`'s t_bs on the same node: 10 OK) |
| 10⁶, 10⁸, 10⁹ size 1 | `POOL_LOG=27/29 ECALC_VERBOSE=2` | identical; 10⁹ 8.4 s, VmHWM 16.5 GB (was 16.2: the 4 × 1 GiB staging + the two seed buffers vs 4 × 2 GiB) |
| 10⁸ size 1 | `ECALC_OVERLAP=0` (the host flow; the seeds streamed synchronously) | identical |
| 10⁸ size 1 | `BS_SEED_CHUNK_MB=8 MN_OUT_CHUNK_MB=8` (many chunks) | identical |
| 10⁸ size 1 | `BS_DEVICE_POOLS=0` (host regions: the old seed path) | identical |
| 10⁷ | `LIMB_BASE=2` | identical (the binary whole-string path unchanged) |
| 10⁸ sizes 2, 3, 4 | `POOL_LOG=27` (size 4 also with `BS_MDEV_LOGL=21`) | identical, every node's VERIFY OK |
| 10⁸ size 2 | `MN_DM=host`, `MN_COMBINE=host` (the stand-in scatter + broadcast paths) | identical |
| 10⁹ sizes 2, 4 | `POOL_LOG=29` (size 4 with `BS_MDEV_LOGL=25`) | identical; 27.1 / 19.6 s, VmHWM 19.3 / 18.9 GB per process |
| 10⁸ checkpoint + restart | `BS_CKPT_DIR BS_CKPT_EVERY=3`, then `BS_RESTART=1` (the 1 GiB staging as the ckpt buffer) | identical |
| **4 × 10¹⁰ size 1**, ×2 (final build) | `ECALC_VERBOSE=2 RNS_VERBOSE=1 ECALC_WINDOWS=ref/windows_4e10.txt`, file on `/tmp`, reference evicted | identical, VERIFY OK, **82.75 / 82.72 s, VmHWM 11.7 GB** |
| 4 × 10¹⁰, `BS_SEED_DIRECT=0` | (every seed chunk by DMA) | identical, 89.0 s, 12.6 GB |
| 4 × 10¹⁰, B1 + the first B2 (DMA thread, null stream), ×2 | job 20763 | identical, 89.2 / 88.4 s, 12.0 GB |

### 4 × 10¹⁰, size 1: before / after

| | merged main (RESULTS §74, 5 runs) | h10 final (2 runs) |
|---|---:|---:|
| init | 16.1 ± 0.9 | 14.3 / 14.4 |
| bs (seeds inside bs / batch / top levels) | 37.3 (— / 23.5 / 13.5) | **37.0 / 37.1** (0.1 / 23.1 / 13.8) |
| dm (reciprocal / division) | 32.9 (16.2 / 16.7) | 31.3 (15.7 / 15.6) |
| phases | 70.3 ± 0.5 | 68.4 / 68.3 |
| **wall (`total`)** | **86.4 ± 1.3** | **82.75 / 82.72** |
| **peak host (VmHWM)** | **48.8 GB** | **11.7 GB** |

(The old bs line hid the seeds' join: I2's seeds were computed in init and copied at the start of bs; the
copy is gone and the thread finishes inside init — 13.2 s of 14.3.) The division: "X out + low product"
7.6 s with no X out; X's residues 0.55 s in the background. The 4 × 10¹⁰ mem_report of the final build:

| phase | device GB (max APU) | planes | regions | pool don / bor / hip | pool live / peak | host RSS | staging | X | digits | other | **HWM** |
|---|---:|---:|---:|---|---|---:|---:|---:|---:|---:|---:|
| init | 216.6 (58.0) | 120.3 | 95.7 | 0 / 0 / 0 | 0 / 0 | 11.5 | 8.6 | 0 | 0 | 3.0 | 11.5 |
| bs | 226.3 (58.0) | 120.3 | 0 | 0 / 95.7 / 9.8 | 35.6 / 83.7 | 9.0 | 4.3 | 0 | 0 | 4.7 | 11.7 |
| recip | 261.9 (66.9) | 120.3 | 0 | 0 / 95.7 / 45.3 | 53.3 / 121.5 | 6.9 | 0 | 0 | 0 | 6.9 | 11.7 |
| dm | 261.9 (66.9) | 120.3 | 0 | 0 / 95.7 / 45.3 | 17.8 / 121.5 | 7.5 | 0 | 0 | 0 | 7.5 | 11.7 |
| end | 120.9 (30.2) | 120.3 | 0 | 0 | 0 / 121.5 | 6.9 | 0 | 0 | 0 | 6.8 | 11.7 |

Against A-mem's table for `main` (RESULTS §74 / results/A-mem.md): host at init 45.7 → 11.5 (staging 42.9 →
8.6: 4 × 1 GiB + the two 2 GiB seed buffers, freed after the seeds), at bs 47.4 → 9.0, at dm 70.8 → 7.5
(X 17.8 → 0, the digit string 40 → 0 already by A-out), **HWM 48.8 → 11.7 GB**. The device side is
unchanged except that the block pool stays mapped through the output stage (the "dm" row shows X's 17.8 GB
live; released at "end"). What remains of the host: the ROCm runtime and the program (≈ 7 GB), the
checkpoints' staging 4.3 GB, the writer's two 256 MB chunks and a 128 MB limb buffer.

## Open issues

- Host peak 11.7 GB at 4 × 10¹⁰: the gate said ≈ 20 GB (B2) — the pinned staging of `rns_init` could go
  to 0 in the decimal device flow (the checkpoint code would malloc its 1 GiB chunk buffers, `ckpt_buf`'s
  fallback) for another 4.3 GB; not done (T owns the checkpoint code).
- `mem_report`'s "dm" row now shows the block pool with X live (released after the output stage); the
  device figure at "dm" is the same as at "recip".
- The seed thread's two buffered chunks (4 GB) are copied by `hipMemcpyAsync` once the regions exist;
  issuing the two calls took 1.5 s at 4 × 10¹⁰ (the HIP runtime's lock is held by the main thread's
  `hipMalloc` of the plane pools) — inside init, harmless; a `hipMemcpy` on the null stream there queued
  behind the plane pools' `hipMemset` (4 GB/s) and pushed the seeds 6 s past init (the first version).
- `BS_SEED_THREADS` and `MEM_PIN` keep their meaning for the old path only; the streamed seeds use every
  thread on one chunk at a time (`omp for` dynamic), which is what the unpinned default did in effect.
- `binsplit_pregrow` runs inside `rns_init` (from the hook) when the overlap is on: the driver's "init:
  region pools %.2f s" line then reports 0 and the arenas' line prints under `ECALC_VERBOSE=2` only
  (`bs_verbose` is set after `rns_init`).
- The stand-in scatter (`mn_out_scatter_standin`) and the residue broadcast remain for the host flows
  (`MN_DM=host`, `MN_COMBINE=host`); they could go with those switches (PLAN §22 E1).
- A ±1 correction at size 1 redoes the whole digit string (never observed; the affected chunk alone would
  need restartable residue/T2 state in `mn_out_run`).

## Files touched outside my list (minimal, commented `Phase 10 H`)

`ecalc/newton_db.c` (`newton_db_x_dev`, `db_add_small`, three lines in `newton_db_divmod_shifted`),
`ecalc/newton.h` (one declaration), `ecalc/dbig.c` (`db_mod_qs` under a mutex, the body renamed
`db_mod_qs_locked`), `ecalc/mem.c/.h` (`mem_dev_copy_async`, `mem_dev_copy_wait`), `ecalc/binsplit.c`
`binsplit_pregrow` (the once-per-run guard, first line). In `ecalc.c` above the output tail: the `Xdev`/`Xm`
declarations next to the bigints, `newton_db_x_dev` set beside the x hook, `db_release_pools` moved after
`out_stage` (size 1 and node 0), the `out_ctx` filled after the sharded division, the dm line prints the
device X's length; the staging request (1 GiB) in the init block.
