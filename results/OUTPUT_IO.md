# OUTPUT_IO — output

Scope: output: streamed chunks, 60-bit packed digits, write scheduling on a network file system, the correction patch, spill and I/O. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| A-out | per-node output streamed in chunks, rank-local T1/T2 (Phase 9, PLAN.md §19: M5 + C1) |
| C05 | M5, the 60-bit packed digit output (`ECALC_OUT_PACKED=2`) |
| O4 | packed output (`ECALC_OUT_PACKED=2`) adoption path, write rate and round trip |
| IO15 | Phase 15 agent IO: output I/O on a network file system, packed digits, write scheduling, the memory guard |
| IO215 | Phase 15 agent IO, second task: per-part conversion of the packed digit files |
| KP15 | the correction patch on packed output (a defect on main, B2 44c1642) |
| S114 | S1 (Phase 14, W1): spill and I/O — E1, E3, E4, E12 of internal apumult study |

## A-out — per-node output streamed in chunks, rank-local T1/T2 (Phase 9, PLAN.md §19: M5 + C1)

(former file: A-out.md)

Branch `a-out` (from `main` @ a75474d). Owned: `ecalc/verify.c/.h`, new `ecalc/mn_out.c/.h`, the
driver's output tail in `ecalc/ecalc.c` (from the point where X is on the host to the end); plus
`ecalc/Makefile` (the new object and test) and the new `ecalc/tests/t_out.c`.

#### What it does

**The digit string never exists whole.** X < 10^(d+1) in base 10^18 has nl = ⌈(d+1)/18⌉ limbs;
the digits are the limbs written from the top, 18 each, the leading `pad = 18 nl − (d+1)` zeros
dropped. A node holding the contiguous limbs [lo, lo+cnt) of X (an `mdb` share, or a range of a
host number) holds the digits [k0, k1) with k0 = max(0, 18 (nl−hi) − pad), k1 = 18 (nl−lo) − pad.
`mn_out_run` formats them from the top down in chunks of `MN_OUT_CHUNK_MB` (256 MB of digits ≈
14.9 M limbs): the chunk's limbs are fetched (a host pointer, or `hipMemcpy` per quarter of the
device share into a pinned buffer), formatted by an OpenMP loop (two 9-digit halves per limb),
residue-checked, T2-windowed and handed to a writer thread that `pwrite`s it (8 segments in
parallel) into the node's part file while the next chunk is formatted (two buffers; the formatter
blocks only when both are in flight). The peak per node is two chunk buffers plus the limb
buffer, ≈ 0.75 GB, instead of 40 GB at 4 × 10¹⁰.

**Part files.** `<outfile>.part<k>`, k = size−1−rank zero-padded to four digits — file order, since
the mdb share of node 0 is the *low* limbs (the last digits): part 0000 (the top node) carries
"2." and the leading digits, the last part (node 0) the digits down to d_out and the newline, the
trailing d − d_out computed digits are formatted (they are in the residue) but not written. So
`cat <outfile>.part* ` (the glob sorts) is byte-identical to the single-file output. At size 1 the
file is `<outfile>` itself, written the same streamed way.

**The digit residue** (T1's `digits == X mod q`): per chunk `vf_digits_mods` (one pass over the
chunk for all eight primes, the 18-digit block parsed once), accumulated by Horner over the
chunks (`vf_digits_join`: D = D·10^len + v) — so a node ends with its digit range as a number mod
q and its length; `mn_out_digit_res` all-gathers (len_r, D_r) and every node joins them top node
first, the same Horner. Compared with X's residues (`tier1_digits_cmp`).

**T2 windows** are checked by the piece the window *ends* in: `tier2_range(s, k0, k1, head, nhead,
…)` checks the windows starting in [k0 − nhead, k1) and ending at or before k1, taking the chars
before k0 from `head` — the previous chunk's last 49 digits inside a node, and for a node's first
chunk the tails of the nodes above it (`mn_out_boundaries`: every node's last min(49, range) digits
from its lowest three limbs, all-gathered before the run, the head assembled nearest node first).
The window table (built-in + `ECALC_WINDOWS`) is loaded once; `tier2` is `tier2_range` over the
whole string with the old summary line.

**T1 rank-local.** `vf_pq_range_mod(a, b, q)` is the term recurrence over [a, b) (`vf_pq_mod(N)` =
[1, N+1), the same chunking, so size 1 computes exactly what it did); every node runs it over its
own range [bs_a0, bs_b1) in the background thread after its seeds (the size-1 hook, now set at
every size), and `mn_out_pq_combine` all-gathers the (P_r, Q_r) and joins them in node order,
P = P_A Q_B + P_B, Q = Q_A Q_B — the tree's rule. X's residues: the share's by the device kernel
(`db_mod_qs` on the share, sh.n = its length) or the host Horner, placed at the share's offset
(`vf_shift_res`: res · B^lo) and summed over the nodes (`mn_out_res_combine`). P, Q, R residues:
node 0's, broadcast (the stand-in; see the hooks). Every node then runs the full T1 identity
(`tier1_res_pq` with all residues precomputed) and prints its own T1/T2/VERIFY lines; node 0 adds
"mn: all n nodes: VERIFY OK" from an all-reduce of the fail flags. The collectives are small host
vectors through `comm_allgather` (device buffers on APU 0; `mn_out_allgather_u64`), so A-comm's
real all-gather is used when it lands.

**The output stage as a function** (`out_stage` in `ecalc.c`): node 0 calls it at the end of the
dm flow, the other nodes right after the gather (where they used to exit), so the dm flow itself is
untouched (A-div's). Size 1: the hook starts the background thread that computes X's residues
(posted through a semaphore: T1 waits only for those) and then runs the chunked writer during the
low product (as before, minus the 40 GB string). `total` is printed after T1, before the writer is
joined — the same boundary as before, when the 40 GB `fwrite` came after `total` — and then the
dc/T2 lines, `wrote` and VERIFY. The formatting compute (≈ 8 s at 4 × 10¹⁰: residues 2.8, format
2.2, digit residue 3.1) hides under the low product; whether the *write* hides depends on the file
system (below). A correction to X after the hook redoes the digits (the file is rewritten). The
binary-limb path (`LIMB_BASE=2`, the radix conversion) keeps the whole-string code.

**The stand-in for A-div's sharded X** (`mn_out_scatter_standin`): after node 0's division, its host
X is scattered over mesh 0 (basis N = X.n, `comm_shard`) into a device share per node (`dbig`,
share length limbs), node 0's own share included; the writer and the residues run on the device
shares exactly as they will on the `mdb` of X. The two `HOOK A-div` comments in `out_stage` say what
plugs in: `src->dev = &Xm.sh` with `mdb_share` for lo/cnt (the scatter goes), and the P, Q, R residues
per share by `db_mod_qs` + `mn_out_res_combine` instead of the broadcast.

#### Tests

Job 20726 (one node, aac6-spx-b), clone `~/ntt-aout` @ b432d9e, script `ecalc/aout_test.sh`
(logs `~/aout/`). Every run VERIFY OK on every node; `cmp` against `ref/e_<digits>.txt` of the file
(size 1) or of `cat` of the sorted part files (size > 1).

| test | command (from `~/ntt-aout/ecalc`) | result |
|---|---|---|
| `tests/t_out` | `srun ... ./tests/t_out` | 2 860 host checks, 0 failures (60 random X, sizes 1–5, chunks of 1–9 limbs, 12 random windows each; the term recurrence over ranges) |
| `tests/t_verify` | | VERIFY OK (334 checks) |
| 10⁸ size 1 | `POOL_LOG=27 ./ecalc 100000000 <out>` | identical, 4.3 s, VmHWM 13.2 GB |
| 10⁸ size 1, `ECALC_OVERLAP=0` | | identical (the synchronous streamed writer) |
| 10⁸ size 1, `MN_OUT_CHUNK_MB=8` | | identical (13 chunks) |
| 10⁸ size 2 / 3 / 4 (one node) | `SLURM_JOB_ID=$J ./mnrun.sh <s> env POOL_LOG=27 ./ecalc 100000000 <out>` | cat of the parts identical; T1/T2 ok on every node (size 4 with `MN_OUT_CHUNK_MB=8`) |
| 10⁸ size 2, `MN_COMBINE=host` | | identical (the M2 combine path ends in the output stage too) |
| 10⁹ size 1 | `POOL_LOG=29` | identical, 8.4 s, VmHWM 16.2 GB |
| 10⁹ size 2 / 4 (one node) | `POOL_LOG=29` | cat of the parts identical; T1/T2 ok on every node; 12.1 / 12.5 s |
| 10⁷ `LIMB_BASE=2` | | identical (the binary whole-string path unchanged) |
| **10⁸ and 10⁹ on 2 real nodes** (job 20740, s24-26 + s24-30, one node-process each) | `SLURM_JOB_ID=$J ./mnrun.sh 2 env POOL_LOG=27/29 ./ecalc <d> ~/aout/e.txt` | cat of the parts identical; T1/T2 ok on both nodes; 10.7 s / 50.5 s (1 GbE) |
| **4 × 10¹⁰ size 1**, output on NFS (`~/aout`) | `ECALC_WINDOWS=ref/windows_4e10.txt ./ecalc 40000000000 ~/aout/e_4e10.txt` (page cache evicted first) | identical to `results/e_4e10.out`, VERIFY OK, 8 windows; peak host **48.7 GB** (was 70.8); with the writer joined before T1 (the first version): total 116.5 s, T1 23.1 s of waiting for the writer — the 40 GB went into the NFS client's cache at 1.4 GB/s (29 s in the writer thread) and `close` flushed it for 339 s |
| **4 × 10¹⁰ size 1**, output on the node's NVMe (`/tmp`) | the same, b432d9e (`total` before the join) | identical, VERIFY OK, **total 95.2 s** (bs 43.3, dm 33.2, T1 0.0, init 18.7), peak host 48.7 GB; the writer thread 77.9 s (0.5 GB/s: the page cache is throttled to the disk's write rate on this node — `dd` to `/tmp` gets 1.3 GB/s), formatting compute 8.2 s, `close` 0.0 s |

Host memory at 4 × 10¹⁰: VmRSS 31.3 GB in the output stage (the host X 17.8 GB + the staging and
pools), VmHWM 48.7 GB — the peak is now the bs phase (47.4 GB during the batch levels: the region
pools' host side), no longer the output; the "≈ 30 GB" of the gate is the output stage's figure.

The multi-node paths exercised: the stand-in scatter (device shares via `db_from_bi`), the
device-share fetch per chunk (`hipMemcpy` per quarter), `db_mod_qs` on the share, the residue
placement and sum, the term recurrence joined over 2–4 nodes, the T2 head over node boundaries
(the windows at 10⁶ and 10⁸ land in different nodes' parts: "2 checked on this node" / "0"), the
digit residue joined over the nodes, `comm_allgather` over the TCP mesh (the day-0 fallback).

#### Open issues

- **The file write on aac6 cannot hide.** 40 GB at 0.5–1.4 GB/s is 30–80 s whatever the target
  (NVMe page cache throttled, or NFS), against a ≈ 10 s low product. The streamed writer bounds the
  memory (two 256 MB chunks + a 128 MB limb buffer) so it cannot run ahead of the disk; `total`
  therefore stops before the join, as the old `fwrite` did, and the dc line reports the wait
  ("waiting for the writer 72.5 s"). On the target machine's parallel file system the write should
  hide under the low product; on aac6 the run's wall clock including the write is ≈ 175 s with a
  local disk (vs. an unmeasured but comparable `fwrite` + `close` before). If a 4 × 10¹⁰ run's
  file is not needed, omit `<outfile>` (nothing is written; the checks still run).
- Three real nodes were not idle together during the session (2-node runs done; 3 node-processes
  on one node cover the non-power-of-two group and the three-way part/T2-head logic).
- **The A-div hooks** (`out_stage`, two `HOOK A-div` comments): X's share from `Xm.sh` with
  `mdb_share` instead of the scatter; P, Q, R residues per share by `db_mod_qs` + `mn_out_res_combine`
  instead of node 0's broadcast. The stand-in scatter (`mn_out_scatter_standin`) can then go.
  Note the size-1 flow keeps X on the host (17.8 GB): with A-div's X on the device the size-1 writer
  can use the device source too (`src->dev`, no host X at all) — a 17.8 GB further drop.
- `mn_out_allgather_u64` allocates its two small device buffers per call (a few calls per run).
- Part naming is file order (`part0000` = the head), i.e. k = size − 1 − rank, since node 0 holds
  the low limbs of an `mdb`; if the integrator prefers `.part<rank>` it is one line in `mn_out_run`.
- `ECALC_OVERLAP=0` at size 1 formats and writes after `total` (synchronously); before, dc and T2
  were inside the timed phases on that path.

#### Files touched outside my list

`ecalc/Makefile` (the `mn_out.o` object, `tests/t_out`), new `ecalc/tests/t_out.c`, and inside
`ecalc.c` above the output tail: the `pq_bg` struct gained the term range `[a0, b1)`, `struct x_bg`
and `struct out_ctx` are declared before `binsplit_e` (the other nodes call `out_stage` at their
two exit points after the gather / the M2 send: one line each), and the M5 hook after the seeds
is set at size > 1 (`else if (mn_size_ > 1) { bs_after_seeds_hook = pq_bg_start; ... }`). The dm
flow between bs and the output is otherwise unchanged.

## C05: M5, the 60-bit packed digit output (`ECALC_OUT_PACKED=2`)

(former file: C05.md)

Session 2026-10-11c wave C, agent c05, branch `c05` (from origin/main bc80001). Times Eastern. Numbers are **measured** (aac6 SH5 CPX node
aac6-sh5cpx-a, one APU, NFS `/shared`, O_DIRECT writer) unless marked modelled / assumed.

#### Result

- **SUCCESS.** The 60-bit packed format is implemented behind `ECALC_OUT_PACKED=2` (off by default; 1 stays the default, 0 = ASCII).
  Digits are byte-identical to the reference in all tests (VERIFY OK on every run; `digcmp.sh` identical).
- **Bytes: -6.25 % exactly** (7.5 B per 18 digits = 0.4167 B/digit against 0.4444): 4e9 digits 1 777 782 789 B -> 1 666 671 677 B on disk (ratio 0.93750,
  including header and the 886-byte `.t1` sidecar); 1e9: 444 449 443 -> 416 671 665 B (measured).
- **Write time at 4e9, single node (NFS, ~0.11 GB/s, writer thread): packed v1 15.80 s -> v2 14.80 s; paired ABBA, 5 rounds (10 + 10 runs),
  v2 - v1 = -1.00 s, 95 % CI +-0.33 s (t, df 4), every round negative** (-0.60, -1.08, -1.11, -0.94, -1.30). The wait for the writer inside `dc` moves by
  the same amount (-0.98 s +-0.27). Format time (`dc` "format") unchanged: -0.002 s +-0.022 (the packer costs nothing visible).
  Total wall: -0.04 s +-0.39 (not significant): at one node the write is overlapped with the run, so the write saving is not on the wall here.
- **Rate check:** the saving is proportional to bytes: 0.111 GB less in 1.00 s = 9.0 s/GB = 0.111 GB/s, the NFS rate of both arms (1.78 GB / 15.8 s = 0.113 GB/s).

#### Model at 576 nodes (modelled; bytes per node from the measured ratio 60/64, rate assumed)

Digits per node d/576; v1 = 8/18 B/digit, v2 = 7.5/18 B/digit. Exposed-write saving = delta bytes / per-node write rate (the rate is an **assumed**
prior, 0.6-0.8 GB/s, not measured at the target).

| size | digits/node | v1 GB | v2 GB | saved GB | at 0.6 GB/s | 0.8 GB/s | 1.0 GB/s |
|---|---|---|---|---|---|---|---|
| 5.276e13 | 9.160e10 | 40.71 | 38.17 | 2.54 | 4.2 s | 3.2 s | 2.5 s |
| 5.167e13 | 8.970e10 | 39.87 | 37.38 | 2.49 | 4.2 s | 3.1 s | 2.5 s |

PLAN §38.4's "about -4 s" is the 0.6-0.65 GB/s end; at 1 GB/s it is -2.5 s. Total bytes of all parts: 23.4 TB -> 22.0 TB (modelled).

#### Design

- `ecalc/packed_fmt.h`: version 2 = same 4096-byte header (`reserved0` = 60, `limb_bytes` stays 8), then the part's limbs (file order, most significant
  first, each < 10^18 < 2^60) as a little-endian 60-bit stream: 15 bytes per pair (`a | b<<60` low 8 bytes, then `b>>4` 7 bytes), a last odd limb 8 bytes;
  `ecp60_bytes(n) = 15 (n/2) + 8 (n&1)`. A pair is byte-aligned, so a part of any limb count (odd boundaries between nodes) is a self-contained stream; random access
  reads from an even file index. Helpers: `ecp60_pack/unpack/put_pair/get_pair`, and `ecp60_stream_chunk` (the writer's chunk step: an odd last limb of a
  non-final chunk is carried to the next chunk).
- `ecalc/mn_out.c`: `out_packed()` returns 0/1/2; the writer packs after the reversal into the free half of the chunk buffer (buffer +16 KiB), the file offset is a
  running counter (the O_DIRECT writer needs a sequential stream already); residues (`packed_mods`), T2 windows and tails read the limbs unchanged, so the checks are the
  same as v1. The K patch (`packed_patch`) reads the pairs that hold the changed limbs, checks them against the old limbs, repacks and calls `patch_bytes`.
  RECHECK (`recheck_packed_hdr/digits`, `recheck_rd_packed`) reads both versions. No other file touched.
- `tools/unpack_digits.c`: reads version 1 and 2 (chunk forced even), `-o`, `--cmp`, `-n`, per-part subsets and the residue check as before; `digcmp.sh` works unchanged on either.
- `ecalc/README.md`: the `ECALC_OUT_PACKED` row extended (value 2).

#### Tests

| test | command / size | result |
|---|---|---|
| pack/unpack round trip n = 0..70, extremes (0, 10^18-1), no overrun; 3000 random chunked streams (chunk sizes 1..200 incl. odd, odd total) == one-shot pack; reads from any even index | `tools/test_pack60` (local and aac6) | PASS |
| unpack_digits both formats: 8 shapes x {v1, v2} + 2 extra tiny v2 (d = 54, 89): parts 1..7 with odd/even limb counts at the part edges, last partial word, d_out cuts, `-o`/stdout/`--cmp`, changed byte, longer reference, per-part subsets, corrupted limb -> residue failure; v1 patched cases | `tools/test_unpack.py` (local) | PASS (all lines ok) |
| 1e8, formats 0/1/2, vs prefix of the 1e9 reference | digcmp | identical x3 |
| 1e9, formats 0/1/1/2/2 vs `e_1000000000.out` | digcmp | identical x5, VERIFY OK |
| 1e9 format 2 with odd writer chunks `MN_OUT_CHUNK_MB=1,5,7` (5 and 7 give odd chunk limb counts: the carry of the odd limb) | digcmp vs reference | identical x3 |
| 511461828, `ECALC_CORR_PATCH=1 ECALC_TEST_CORR=14` (the K patch on a packed part), formats 1 and 2 vs the format-0 file of the same run | digcmp | identical, VERIFY OK, patch of 1 part file in both |
| RECHECK on the packed files (1e8: formats 0, 1, 2) | `ECALC_RECHECK=1 ./ecalc 100000000 file` | RECHECK OK x3 |
| 4e9, format 0 (ASCII, reference; its first 1e9 digits == `e_1000000000.out`), then formats 1 and 2 x 10 runs, all VERIFY OK | digcmp vs the ASCII file | identical x20 |

Setting: `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30` (T11), `ECALC_OUT_MODE` default = O_DIRECT on NFS. Drivers: `archive:drivers/c05/c05_run.sh`, `c05_run2.sh`, `c05_run3.sh`.
The 4e9 reference is ecalc's own ASCII output of the same build (no independent 4e9 reference exists on aac6); its prefix matches the 1e9 reference and the run is VERIFY OK.

#### Open issues / not done

- **Multi-node writer not exercised** (no aac7). The per-part code path is the same function; the python test covers 1..7 parts and the odd-limb part edges of the reader, and the writer
  was exercised with odd chunk sizes, but a 2+ node `ECALC_OUT_PACKED=2` run (incl. `MN_OUT_EARLY`/`MN_OUT_DKM_HI` with the split chunk, `ECALC_CORR_PATCH` at size > 1) is untested: needs one aac7 item
  (`ECALC_OUT_PACKED=2`, 2 nodes, 1e9..1e10, digcmp + RECHECK), a few minutes.
- The write rate at the target (the prior 0.6-0.8 GB/s) decides the 576-node saving: 2.5-4.2 s. Nothing here measures it.
- Reader cost: `unpack_digits` of v2 adds one unpack pass per chunk (not timed separately; the 4e9 compare takes ~5 s for both versions).
- Adoption (user): changes the on-disk format of runs that set `ECALC_OUT_PACKED=2`; anything outside the repo that reads `ECPACK18` version 1 only must be updated (headers carry version 2).

#### RESUME

Branch `c05` pushed (commits: implementation, README row, results). To merge: coordinator review of `ecalc/mn_out.c` (writer branch under `if (w->packed == 2)`, `packed_patch`,
recheck readers), `ecalc/packed_fmt.h`, `tools/unpack_digits.c`, `tools/test_*`. To close the multi-node gap: queue an aac7 item as above. aac6: clone `~/ntt-c05` and `~/sess1011c/c05/` outputs removed/left only as logs.

#### aac7 multi-node check (queued 2026-10-11, coordinator-approved)

Queue item `~/aac7q/queue/46_c05_packed2.sh` (copy: `archive:drivers/c05/46_c05_packed2.sh`; 2 nodes, EST 15 min; STUB dry run OK). Build: `~/ntt-wt/c05` at 5248c31, SHMEM build on the login node
(`make -B SHMEM_CRAY=1 GMP_HOME=~/gmp`, log `~/sess1011c/c05_aac7_build.log` shows `-DCOMM_SHMEM`). Steps: 1e9 format 2 (digcmp vs `~/ref/e_1000000000.txt`, then RECHECK) and format 1;
108388422 digits with `ECALC_CORR_PATCH=1 ECALC_TEST_CORR=36` in formats 0/1/2 (1 and 2 compared with 0, RECHECK on 2); 1e10 formats 2 and 1 vs the prefix of `~/ref/e_1e11.out` if the first part took < 9 min.
(The coordinator suggested TEST_CORR=14; at size 2 the KP/EW tests used 108388422 with 36, since 14 is the 511461828 single-node case: kept to the proven pairing.)
**Collect:** `ssh aac7 'cat ~/sess1011c/c05/aac7/summary.txt'` (logs `~/sess1011c/c05/aac7/*.log`; outputs removed by the item). Pass = every `digcmp` identical, `VERIFY OK` count = 2 per run, `RECHECK ... OK`, no step left.
Not yet run at the time of writing: results to be added to this file by whoever collects.

#### aac7 multi-node result (collected by f5, 2026-10-11 21:10 ET; items 46 and 73, 2 nodes, build 5248c31, summaries `~/sess1011c/c05/aac7/summary.txt`, `summary2.txt`; measured)
- 1e9, format 2 and format 1: 3 x VERIFY OK each, `digcmp` identical to `e_1000000000.txt`; RECHECK OK on both nodes (format 2). Total 28.9 s (f2) and 29.5 s (f1); writer thread 1.9-2.0 s (f2) against 1.9-4.1 s (f1).
- 1e10, formats 2 and 1: VERIFY OK, `digcmp` identical to the 1e10 reference (4 166 679 852 B packed-2 vs 4 444 457 630 B packed-1: -6.25 % bytes); total 65.2 s (f2) / 62.5 s (f1), single runs, no timing claim; writer 25.1 s (f2) / 26.6 s (f1).
- Patch at size 2 (`ECALC_CORR_PATCH=1 ECALC_TEST_CORR=36`, 108 388 422 digits, forced correction: "corrections 0/36", the part file redone on both nodes): item 46 printed `DIFFERS` for f1/f2 vs f0, which was a harness error (the format-0 output is `out.c_f0.part000N`, not `out.c_f0`; the compared path did not exist). Item 73 repeated it with the f0 parts concatenated (108 388 425 B): **`digcmp` f1 and f2 identical to f0, RECHECK OK on both nodes for f1 and f2, 3 x VERIFY OK and 2 x "redoing the part file" in each run.**
- Status: the multi-node gap in "Open issues" is closed for 2 nodes (1e9..1e10, patch path). Still untested: more than 2 nodes and the target write rate.

## O4: packed output (`ECALC_OUT_PACKED=2`) adoption path, write rate and round trip

(former file: O4.md)

Session 2026-10-11/12 overnight, branch `o4` (from main 0298fe2). Times Eastern. **Status: drivers armed; measurements pending (see RESUME).**

#### 1. What exists vs what adoption needs (checked in the code and results/OUTPUT_IO.md §C05; no ecalc code change was needed)

| need | state | evidence |
|---|---|---|
| end-to-end write in `ECALC_OUT_PACKED=2` (1 node, multi-node, odd chunk sizes, K patch) | exists, off by default | C05: 1e8..4e9 on aac6; 2 nodes 1e9/1e10 and the forced-patch case on aac7 (m) |
| unpack tool producing byte-identical decimal text | exists: `tools/unpack_digits -o`, per-part subsets (parts concatenated in order = the file) | `tools/test_unpack.py`; C05 compared via `--cmp`. **Gap:** no run had yet done `-o` text + `cmp` on real multi-node parts: O4 does it (`o4_rate.sh check`) |
| digcmp on the unpacked output | `ecalc/digcmp.sh` reads either form (`unpack_digits --cmp`) | C05 |
| checkpoint / recheck compatibility | RECHECK reads v1 and v2 (C05, 1e8 and aac7 2 nodes); `ECALC_CHECKPOINT=1` was set in the C05 aac7 runs | **Gap:** RECHECK against the checkpoint set (not the sidecar) on 4 nodes: O4 ckpt step |
| write rate | **Gap** (the only open item of C05 for the 576-node saving): no run printed bytes / writer-thread seconds per arm side by side | `ecalc` already prints `wrote ... (G GB; write T s in the writer thread ...)`; `tools/o4_rate.sh` parses it |

#### 2. Tools added (all behind existing switches; no default changes)

- `tools/o4_rate.sh`: inside a hold, runs ASCII (0) / packed 1 / 60-bit packed 2 in rotated order, per run: VERIFY OK count, bytes written (all nodes), writer-thread time per node, rate = GB / s per node (mean over nodes), `total`, wall; then digcmp vs the reference, `unpack_digits -o` per part (parallel) + `cat` + `cmp` against the reference (fmt 1, 2), `ECALC_RECHECK=1` on the first fmt-2 run; deletes the digit files; writes `rates.tsv`, `summary.txt`, `O4_DONE`.
- `tools/o4_aac6.sh <CPX|SPX> <HH:MM ET>`: aac6 wrapper (own 45-min hold, cancelled at the end, waits until the start time).
- `tools/o4_aac7_item.sh`: aac7 queue item 80 (4 nodes, EST 45 min, APPROVED).

#### 3. Armed runs (no results yet)

| run | where / when | content | output |
|---|---|---|---|
| aac6 CPX | node aac6-sh5cpx-b, start 03:31 ET, <= 45 min | 1e9, 3 arms x 2 rounds, ref `e_1000000000.out` | `~/o4_CPX/summary.txt`, `rates.tsv`, `O4_DONE` |
| aac6 SPX | any PPAC SPX node, start 05:35 ET (after H4's 03:35-05:30 holds; abandoned with ALERT if no node in 20 min), <= 45 min | 1e9 and 1e10 (reference for 1e10 = the run's own ASCII output, whose first 1e9 digits are compared with the 1e9 reference; no 1e10 reference on aac6) | `~/o4_SPX/...` |
| aac7 item 80 | 4 nodes, queue order after 75 | 1e9 and 1e10 (exact prefix of `~/ref/e_1e11.out`), 2 rounds x 3 arms, then 1e9 with `ECALC_CHECKPOINT=1` | `~/o4_aac7/summary.txt`, `main/rates.tsv`, `ckpt/...`, `O4_DONE` |

Stats note: 2 runs per arm per size: write time and rate are reported as means with the per-run values in `rates.tsv`; this is **not** a timing claim (no CI). A rate claim needs more rounds; the packed saving is bytes / rate (C05: -6.25 % bytes exactly).

#### 4. Collected results (aac7 4-node item 80 and aac6 CPX node; 2026-10-12; all numbers measured)

Format 0 = ASCII, 1 = packed (`ECALC_OUT_PACKED=1`), 2 = 60-bit packed (`=2`). Write rate = GB per node divided by the writer-thread time (mean over nodes; the file system is shared and other jobs ran).

| where | digits | fmt | bytes (GB, all nodes) | write time (s) | rate (GB/s per node) | ecalc total (s) | n |
|---|---|---|---|---|---|---|---|
| aac7, 4 nodes | 1e9 | 0 | 1.00 | 3.23 | 0.083 | 26.0 | 2 |
| aac7, 4 nodes | 1e9 | 1 | 0.44 | 1.14 | 0.103 | 21.3 | 2 |
| aac7, 4 nodes | 1e9 | 2 | 0.40 | 1.41 | 0.092 | 23.0 | 2 |
| aac7, 4 nodes (ckpt step) | 1e9 | 0 / 1 / 2 | 1.00 / 0.44 / 0.40 | 2.52 / 1.03 / 0.90 | 0.101 / 0.107 / 0.111 | 23.3 / 19.8 / 19.2 | 1 |
| aac7, 4 nodes | 1e10 | 0 | 10.00 | 40.62 | 0.062 | 73.4 | 1 |
| aac7, 4 nodes | 1e10 | 1 | 4.44 | 10.50 | 0.107 | 36.8 | 1 |
| aac7, 4 nodes | 1e10 | 2 | 4.16 | 8.98 | 0.116 | 39.4 | 1 |
| aac6 CPX, 1 node | 1e9 | 0 | 1.00 | 8.88 | 0.113 | 8.5 | 2 |
| aac6 CPX, 1 node | 1e9 | 1 | 0.44 | 3.88 | 0.114 | 8.2 | 2 |
| aac6 CPX, 1 node | 1e9 | 2 | 0.42 | 3.69 | 0.114 | 8.8 | 2 |

- Checks at 1e9 (aac7 4 nodes and aac6 CPX): digcmp identical for every format; `unpack_digits -o` of the packed parts + `cat` byte-identical to the reference (cmp) for formats 1 and 2; VERIFY OK in every run (5 lines at 4 nodes, 1 at CPX).
  `ECALC_RECHECK=1` on the format-2 parts: **RECHECK OK on all 4 nodes** (aac7, 1e9 and 1e10) and OK on CPX. The driver's "4 FAIL lines" count is a false positive: its pattern matches libfabric `Failed ... ofi_monitor_subscribe` warnings, not RECHECK output (the RECHECK lines say "RECHECK OK"). The recheck used the residue form (no top-level set, P and Q from the sidecar).
- **1e10 digit comparison was not made**: the driver's reference path for 1e10 was wrong (`ref10.txt: No such file or directory`, path doubled), so `digcmp ... DIFFERS` and `UNPACK TEXT DIFFERS` at 1e10 are artefacts of the missing file, not digit differences. VERIFY OK holds for the three 1e10 runs and RECHECK OK for the format-2 run. A 1e10 byte comparison is still open (needs the reference at the right path).
- The 1e10 item stopped after round 1 (budget), so 1e10 numbers are single runs; the 1e9 means are n = 2. No ABBA statistics (these are rate measurements, not a timing claim).
- Reading: packed output writes 0.42 (fmt 1) / 0.40-0.42 (fmt 2) of the ASCII bytes, so the write time at 1e10 falls from 40.6 s to 10.5 s (fmt 1) / 9.0 s (fmt 2) per run and the ecalc total from 73.4 s to 36.8 / 39.4 s (single runs; the ASCII 1e10 write is the long pole on this file system). The per-node rate is 0.06-0.12 GB/s on aac7 and 0.11 GB/s on CPX: file-system bound, not format bound.
- Pass criteria (every digcmp identical at 1e9; every unpack byte-identical; VERIFY OK; RECHECK OK; no step left): met at 1e9; open for the 1e10 byte comparison (reference path). aac6 SPX (`~/o4_SPX`) not collected here.

**4.1 The 1e10 byte comparison (aac7 item 81, `tools/o4_aac7_ref10.sh`, 4 nodes, 2026-10-12 04:39-04:54 Eastern; all measured)**
Reference: the first 10000000002 bytes of the exact 1e11 reference file plus a newline (the 1e10 prefix; path fixed from item 80). One round, VERIFY OK 5 lines per run, rc 0.

| digits | fmt | bytes (GB) | write time (s) | rate (GB/s per node) | ecalc total (s) | digcmp vs 1e10 prefix |
|---|---|---|---|---|---|---|
| 1e10 | 0 (ASCII) | 10.00 | 27.93 | 0.090 | 54.25 | **identical** (10000000003 bytes on disk) |
| 1e10 | 2 (60-bit packed) | 4.16 | 18.59 | 0.056 | 45.50 | **identical** (4166683063 bytes on disk) |

- `unpack_digits -o` of the four packed parts (parallel, 164 s) + `cat` is **byte-identical (cmp)** to the 1e10 prefix: the packed round trip is exact at 1e10. Item 81 counts 3 identical, 0 DIFFERS.
- This closes the open 1e10 criterion of section 4: all pass criteria are met (digcmp identical at 1e9 and 1e10; unpack byte-identical; VERIFY OK; RECHECK OK).
- Rate note: the packed write moves 0.42 x the ASCII bytes, but in this run its writer time was 18.6 s vs 27.9 s (0.67 x), not 0.42 x, and the per-node rate is lower (0.056 vs 0.090 GB/s): at 1e10 the shared file system, not the byte count, set the write time in these single runs (item 80 measured 40.6 / 9.0 s, so the file-system speed varies by 1.5-2 x between runs). The ecalc total is 45.5 s packed vs 54.3 s ASCII here (single runs, no statistics).

#### RESUME

Collect: `ssh aac6 'cat ~/o4_CPX/summary.txt ~/o4_SPX/summary.txt'` and `ssh aac7 'cat ~/o4_aac7/summary.txt'`. Pass = every `digcmp` identical, every "unpack -o ... byte-identical", VERIFY OK per run (aac7 4 nodes: 2 per run in C05 counting), RECHECK OK, no step left. Then fill sections here: the table of write rate (GB/s per node) and write time per arm and size, the 576-node saving at the measured rate (C05 model: 2.54 GB per node saved), and cleanup: `rm -rf ~/ntt-o4 ~/o4_*` on aac6, `~/ntt-wt/o4` on aac7. aac6 digit files are deleted by the driver; NFS is shared between the aac6 write rates and other jobs (note the load).

## IO15 — Phase 15 agent IO: output I/O on a network file system, packed digits, write scheduling, the memory guard

(former file: IO15.md)

PLAN.md §36.4 row 1f. Branch `p15-IO` from int15 0b80ca3 (B0). aac6 clone `~/ntt-IO15`, logs `~/IO15/<batch>/`.
Times Eastern. Numbers are labelled **measured** / **modelled** / **assumed**. Every new behavior is behind a switch, off
by default (README rows); the digits are unchanged by default.

#### 0. Summary (for the user's decisions)

| item | switch (off by default) | verdict | evidence |
|---|---|---|---|
| W1 write mode on a network FS | `ECALC_OUT_MODE`, `MN_OUT_THREADS` | **keep O_DIRECT** (the Phase 14 default): the fastest form on aac6's NFS from both a compute node (0.116 GB/s = the 1 GbE link) and the login node (0.64–0.96 vs 0.36–0.48 GB/s buffered + fsync, 10 GbE); buffered forms hold up to 10.7 GB of page cache (HBM) | measured, §3 |
| W2 packed part file | `ECALC_OUT_PACKED=1` + `tools/unpack_digits` | **ready for D4**: 10¹¹ identical after conversion (sha1), RECHECK OK; 2.25× fewer bytes; at 10¹¹ local −25.7 s without the write, −57 s with it; on NFS the with-write wall 110.8 → 65.3 s at 10¹⁰ | measured, §2.2–2.3, §3.2 |
| W5d part file during the division (multi-node) | `MN_OUT_EARLY=1` | works, identical at 10⁹ (2, 4 procs, 2 real nodes), 3 × 10⁹ (2 nodes) and 10¹⁰ (2 procs); `dc` → 0 everywhere; the net gain depends on whether storage shares the fabric's link (aac6 1 GbE: −2.9 s of 557; 2 procs on one node at 10¹⁰: −7 s of 116); **recommend on for the target, confirm at step 3** | measured, §2.1, §2.5, §2.6 |
| W5b waves | `MN_OUT_WAVES=<n>` | works (size 2–4, 2 nodes); at 2 nodes it only serializes (+14.4 s); a target tool if 5(b) shows the aggregate saturating | measured (function, cost); benefit unmeasured |
| W5a stripe layout | `MN_OUT_STRIPE` | command path tested with a fake `lfs`; **unmeasured on Lustre** | tested off Lustre |
| W7 O_DIRECT by FS type | `ECALC_ODIRECT=auto` | works (`local`, `NFS` detected, both O_DIRECT); Lustre → O_DIRECT is **assumed** | measured detection |
| W6 top set at the target | — | run without it (default) or `=2`; RECHECK keeps its residue form (§5) | analysis |
| N3 memory guard | `ECALC_MEM_GUARD_GB` | works: rc 9, one line naming rank, host, phase | measured, §2.1 |

**The 576-node estimate (modelled, `mn_model.py --D 7.38e10 --g 576`)**: 4.25 × 10¹³ digits; without the disk write ≈ 238 s
(4.0 min); with the ASCII part file at an assumed 2 GB/s 260 s (4.3 min), at the measured single-stream Lustre rate
0.6–0.8 GB/s 316–347 s (5.3–5.8 min); **packed at 0.6–0.8 GB/s 264–278 s (4.4–4.6 min)**; all assuming each node gets its
single-stream rate at once (the aggregate is unmeasured: TARGET §6 item 5(b)). The model's exposed write assumes the
low-product overlap that only `MN_OUT_EARLY=1` gives at size > 1 (MD15); without it the whole write is exposed:
+123 s ASCII / +55 s packed at 0.6 GB/s (modelled).

#### 1. What was built (design)

##### W1 — the writer's modes and threads (`mn_out.c`: `out_mode`, `writer_run`, `mn_out_finish`)

`ECALC_OUT_MODE` = `direct` (O_DIRECT, the Phase 14 default), `buffered` (page cache, no sync — the pre-Phase-14 form),
`sync` (buffered, `fsync` + `posix_fadvise(DONTNEED)` at close), `drop` (buffered, `fdatasync` + `DONTNEED` after every
chunk: the page cache — HBM on the APU — never holds more than a chunk), `auto` (W7). Unset: the Phase 14 rule, unchanged.
`MN_OUT_THREADS` (8): the pwrite threads per chunk. `MN_OUT_CHUNK_MB` already existed.

##### W7 — O_DIRECT by file-system type (`spill.c`: `sp_odirect_auto`, `sp_fs_kind`, `sp_fs_name`, `sp_direct_for`, `sp_direct_by_fs`; `spf_open`)

`ECALC_ODIRECT=auto`: every file opened through `spf_open` (checkpoint sets, tree sets, the top set, spills) and the part
file choose O_DIRECT by `statfs` f_type: local disks (ext2/3/4 0xEF53, xfs, btrfs, zfs) O_DIRECT; NFS 0x6969 (and
SMB/CIFS) O_DIRECT — set from W1's measurement (§3: O_DIRECT is the faster form on both aac6 paths; `ECALC_ODIRECT_NFS=0`
gives buffered + `fsync` + `DONTNEED`); **Lustre 0x0BD00BD0 O_DIRECT — an assumption**
(large aligned direct I/O is Lustre's documented fast path, but nothing here measured it; `ECALC_ODIRECT_LUSTRE=0`
overrides; TARGET §6 item 5(a) measures it); tmpfs/ramfs buffered (they refuse O_DIRECT, and are memory); others
O_DIRECT (`ECALC_ODIRECT_OTHER`). `ECALC_OUT_MODE=auto` applies the same rule to the part file alone.

##### W2 — the packed part file (`packed_fmt.h`; `mn_out.c`: `out_packed`, `packed_mods`, `packed_digits`, `packed_hdr_write`, the packed branch of `out_run_one`, `mn_out_finish`; RECHECK: `recheck_packed_hdr`, `recheck_packed_digits`, `recheck_rd_packed`)

`ECALC_OUT_PACKED=1`: the file (same names: `<outfile>`, `<outfile>.part<k>`) is a 4096-byte header and then the node's
base-10¹⁸ limbs as they are, 8 bytes little-endian, **most significant limb first**: 0.444 B/digit. The header
(`packed_fmt.h`, magic `ECPACK18`, version 1): endianness marker, limb size/digits/order, part index and count, d, d_out,
X's limb count and top pad, the part's limb range [lo, hi) and digit range [k0, k1), a `complete` flag, the eight T1
primes and **the part's digit residues** — written with complete = 0 at open and rewritten at close; a text description
at offset 1024. No ASCII formatting in the run: the chunk's limbs are reversed into the write buffer.

The run's verification stays whole on this path:
- **T1 `digits == X mod q`**: the chunk's residues are computed from the bytes handed to the writer (Horner over the
  reversed limbs in base 10¹⁸, OpenMP), joined over chunks and nodes exactly as the ASCII digit residues are (the value of
  the digits [ck0, ck1) equals the limbs' value: the top limb's pad zeros lead), and compared with X's residues from the
  device kernel as before;
- **T2 windows**: the windows that end in the chunk (the same rule `tier2_range` applies) are checked from the digits
  they cover, formatted from those few limbs (`packed_digits`), with the carried 49-digit head; `verify.c` got one small
  accessor, `tier2_window_offsets` (outside my files: minimal, commented);
- the first/last digits and the Phase 11 tail come from `packed_digits` too.

`tools/unpack_digits` (C, gcc -O3 -fopenmp, no ROCm; `make -C tools`): a reader thread, OpenMP formatting, a writer/
comparer thread. `unpack_digits <parts…>` writes the exact ASCII to stdout (`| sha1sum`), `-o <file>` to a file,
`--cmp <ref>` compares with an ASCII reference without writing (cmp's answer: first differing byte and line, or EOF on
the shorter), `-n` only checks. Parts in any order: sorted by the header's index, their limb ranges must join. Every
part's digits, as formatted by the converter, are checked mod the eight primes against the residues the run stored —
the converter checks its own formatting and the file's bytes. `tools/test_unpack.py` writes random packed parts the way
the run does (1–7 parts, d_out below d, chunk boundaries) and checks -o / stdout / --cmp byte for byte, a changed byte, a
longer reference and a corrupted limb: **PASS** (measured, login node and aac6).

`ECALC_RECHECK=1` reads packed parts (detected by the magic): the digit count and the 49-digit tail from the header and a
few limbs, the one pass formats the limbs chunk by chunk and runs the same residue and window checks.

##### W5 — write scheduling (`mn_out.c`: `out_stripe`, `mn_out_run`, `writer_drain`, `mn_out_early_*`; `newton_db.c`, `ecalc.c`)

- **(a) stripe layout**: `MN_OUT_STRIPE=<count>[:<MB>[:<OSTs>]]` creates each part file with that layout before opening
  it — `lfs setstripe -c … -S …M [-i (part × count) mod OSTs]` via `posix_spawnp`, or `llapi_file_create` when built with
  `-DMN_OUT_LLAPI -llustreapi`. Only on Lustre (`MN_OUT_STRIPE_FORCE=1` for the command-path test off Lustre, with a fake
  `lfs`). The runbook alternative, no code: `lfs setstripe -c <count> -S <MB>M <outdir>` on the output directory before
  the run (new files inherit it) — TARGET §6 item 5(c).
- **(b) waves**: `MN_OUT_WAVES=<n>`: the ranks in waves of ⌈size/n⌉ in rank order; a wave formats, checks and writes its
  ranges completely (`writer_drain`: both buffers back, `fdatasync` for buffered forms) while the others wait at a
  barrier (an all-gather over mesh 0, the communicator `mn_out_boundaries` hands in); n barriers on every rank. The
  waiting ranks keep X's share on the device (it is there until the output stage ends anyway).
- **(c) the order of the top set and the part file within a node**: see §4 (analysis; no code).
- **(d) (the integrator's addition, from MD15)**: `MN_OUT_EARLY=1`: at size > 1 the part file was written only after T1,
  so none of it hid under the division (MD15: 42.8 of 43.4 s of `dc` waiting for the writer on 2 nodes). The division now
  calls `newton_mn_x_hook` with X over the group right after `X = (A_h mu) >> …`, before the low product (`newton_db.c`,
  `newton_mn_divmod`, one line + the hook variable; `newton.h`); ecalc.c's `mn_early_hook` calls `mn_out_early_start`,
  which all-gathers the T2 tails (a collective: every rank is at the hook) and starts a background thread running
  `mn_out_run` over this rank's share on the device. `out_stage` joins it (`mn_out_early_join`); if the division
  corrected X after the hook (`newton_st` corrections, the same on every rank) the part file is redone, as at size 1
  (agent K's patch path will replace the redo). Waves are ignored with it (no barriers in the background). Functions
  touched outside mn_out.c: `newton_mn_divmod` (one hook call), `out_stage` (the `c->early` branch and the copy before
  `mn_out_early_free`), `main` (the hook set around `newton_mn_divmod`), new `mn_early_hook`; struct `out_ctx` two fields.

##### N3 — the memory guard (`memsample.c`: `sample_once`, `mem_sampler_start`, `mem_sampler_stop`; `memsample.h`)

`ECALC_MEM_GUARD_GB=<x>`: each sample compares MemAvailable with x; below it the sampler calls `ec_fatal(EC_RC_MEMGUARD
= 9, …)`: one line `ecalc: FATAL [rank r/n host h, APU d, thread t, after <phase>, rc 9]: memory guard …` with MemAvailable,
MemFree, Cached, Dirty, RSS and the pool's live bytes, then `_exit(9)` (the fatal path's rules: first caller only,
watchdog). The phase is the last `mem_report` boundary (`ec_fatal_phase`). Without `ECALC_MEM_SAMPLE` the sampler runs
silent for the guard every `ECALC_MEM_GUARD_PERIOD` s (1). rc 9 is defined in `memsample.h` (fatal.h's list has 3, 6, 8).

##### W6 — the top set at the target (`mn_out_recheck`: a message)

See §5. Code: RECHECK now says when it runs in the residue form (no top set).

#### 2. Tests (all measured on aac6; logs `~/IO15/<batch>/<tag>.log`; batch files in `tools/node/`)

Batch letters: the functional batch ran as **E** (job 21491, s24-26, 2026-09-27 02:15–02:24 EDT, binary 9fdcb8d), the W1
sweep as **B** (21473, s24-30, 01:10–01:33), the 10¹¹ pair as **C** (s24-30, 01:42–02:02), V2's checkpoint item as **Eck**
(21494, s24-16, 02:27–02:53), the 10¹¹ to NFS as **D** (21496, s24-30, 02:53–03:23). (The first functional attempt, A, ran
nothing: `timeout` cannot run a shell function; fixed in job.sh. The first 2-node batch W failed at `srun`: a 2-node job
with `--gpus=4` has 4 GPUs in total; job.sh now asks `--gpus-per-node=4`.)

##### 2.1 Gates and function (batch E, 10⁹ unless noted)

| test | command (in `ecalc/`) | result |
|---|---|---|
| e9, decimal | `./ecalc 1000000000 /tmp/f; cmp f ref/e_1000000000.txt` | VERIFY OK, **IDENTICAL**, total 11.4 s |
| e9, binary | `LIMB_BASE=2 ./ecalc …` | VERIFY OK, **IDENTICAL** |
| packed, O_DIRECT, top set | `ECALC_OUT_PACKED=1 ECALC_CKPT_TOP=1 ./ecalc 1e9 f; unpack_digits --cmp ref f; ECALC_RECHECK=1 ./ecalc 1e9 f` | VERIFY OK (4 windows, digits == X mod q ok); 0.44 GB; **identical**; RECHECK OK (P, Q from the tree level 0 set) |
| packed, `ECALC_OUT_MODE=sync` | + `unpack_digits f \| sha1sum` | identical; sha1 bf9f4b08… = `ref/e_1000000000.txt.sha1`; RECHECK OK (residue form, P, Q from the sidecar) |
| 1 MB chunks (954 chunks: windows across chunk edges) | `MN_OUT_CHUNK_MB=1`, ASCII and packed | both 4 windows ok, VERIFY OK; ASCII IDENTICAL, packed identical |
| packed 10⁶, 10⁷, 10⁸ | `unpack_digits --cmp ref/e_<n>.txt` | VERIFY OK, identical ×3 |
| size 2 packed + top set | `mnrun.sh 2 env ECALC_OUT_PACKED=1 ECALC_CKPT_TOP=1 …`; `unpack_digits --cmp ref f.part*`; RECHECK at size 2 | all VERIFY OK; identical (2 parts); RECHECK OK on both nodes |
| size 3, `MN_OUT_WAVES=3`, packed | `mnrun.sh 3 …`; `--cmp` | VERIFY OK; identical (3 parts) |
| size 4, waves | `mnrun.sh 4 env MN_OUT_WAVES=2 …` | **rc 8** — the budget check (4 ranks × the default 2³¹ planes = 542 GB > 480); rerun at POOL_LOG=29 as mnaccept does: batch F |
| stripe command path | `mnrun.sh 2 env PATH=fakebin:$PATH MN_OUT_STRIPE=4:16:8 MN_OUT_STRIPE_FORCE=1 … 1e8` | `lfs setstripe -c 4 -S 16M -i 0 f.part0000`, `-i 4 f.part0001` (the fake lfs's log); IDENTICAL |
| W7 | `ECALC_ODIRECT=auto ECALC_CKPT_TOP=1 ./ecalc 1e8` to /tmp and to NFS | `file system local` / `file system NFS`, both O_DIRECT; IDENTICAL ×2 |
| N3, immediate | `ECALC_MEM_GUARD_GB=100000 ./ecalc 1e9` | `ecalc: FATAL [rank 0/1 host aac6-spx-b, APU 0, thread 0 (tid …), after start, rc 9]: memory guard (ECALC_MEM_GUARD_GB=100000.0): MemAvailable 528.97 GB is below it on rank 0 at 0.0 s (…)`, **rc 9** |
| N3, during the run | `ECALC_MEM_GUARD_GB=<MemAvailable − 40> ECALC_MEM_SAMPLE=1 ./ecalc 1e10` | stopped at 5.0 s (MemAvailable 469.4 < 476 GB while init mapped the pools), rc 9, "after start" (init's `mem_report` comes at its end, so the phase named is the last boundary passed) |
| N3, size 2 | `mnrun.sh 2 env ECALC_MEM_GUARD_GB=100000 …` | both ranks print their line (rank 0/2, 1/2), rc 9 |
| W5d, size 2 | `mnrun.sh 2 env MN_OUT_EARLY=1 … 1e9` | "the part file started in the division (MN_OUT_EARLY): its writer ran 0.24 s, joined after 0.00 s"; VERIFY OK; IDENTICAL |
| W5d, size 2 at 10¹⁰ (paired) | `mnrun.sh 2 … 1e10` without / with `MN_OUT_EARLY=1` | both parts' sha1 3fc02547… = `e_1e10.out`'s; `dc` 3.89 → **0.00 s**, total 115.6 → 108.5 s (one run each; the 3.9 s of writer wait is gone, the rest is run-to-run noise) |

##### 2.2 W2 at 10¹¹ on the local disk (batch C, s24-30, paired; measured)

| | ASCII (a11) | packed (p11) |
|---|---|---|
| `total` (no write; D3's first wall) | 243.61 s (T1 22.6) | **217.89 s** (T1 0.7) |
| process wall (with the write) | 304.7 s | **247.6 s** |
| file | 100.00 GB, writer 59.05 s (1.69 GB/s) | 44.44 GB, writer 27.42 s (1.62 GB/s) |
| identical to `e_1e11.out` | sha1 578f5efb… (`dd iflag=direct`, 102 s) | `unpack_digits f \| sha1sum` = 578f5efb… (72.8 s) |
| RECHECK (residue form) | OK, file pass 156.9 s | OK, file pass **77.9 s** |

Both runs had two corrections (`corrections 0/2`): at size 1 a correction after the hook makes T1 join the writer and
the file be rewritten (V2's finding; agent K's row). With ASCII that costs 22.6 s of T1; the packed rewrite (44 GB) hides
under the rest (0.7 s). **Packed: −25.7 s without the write, −57 s with it, at 10¹¹ on the local NVMe** (one run each).
The converter (measured, same node, local NVMe): to a pipe (`| sha1sum`) 72.8 s (reads 44.4 GB at 1.66 GB/s, formatting
2.6 s, residues 4.4 s; the pipe and sha1sum are the limit); residue check only (`-n`) 25.5 s (1.74 GB/s, the disk's read
rate); to a file (`-o`, 100 GB buffered, then fsync) 179 s (0.56 GB/s written). Memory: MemAvailable min 134.5
(packed) vs 134.7 GB (ASCII) — no difference; Cached ≤ 0.66 GB in both.

##### 2.3 10¹¹ packed to aac6's NFS (batch D; measured)

`ECALC_OUT_PACKED=1 ./ecalc 1e11 ~/IO15/w11/f` (O_DIRECT): total 576.8 s, wall **992.6 s**, 44.44 GB, writer 407.0 s
(0.109 GB/s: the 1 GbE link). This run also had two corrections: T1 waited 359.6 s for the first write, then the file
was written again (the with-write wall holds two writes: ≈ 2 × 44.4 GB at 0.11 GB/s). The ASCII reference run of Phase 14
(T1 841 s for 100 GB) was the same story at 2.25× the bytes. Converted from NFS: `unpack_digits f | sha1sum` =
**578f5efb…** (identical), 395.7 s (the NFS read, 0.12 GB/s); RECHECK OK in 397 s (file pass 390.7 s). File removed.

##### 2.4 V2's item D: checkpoints with O_DIRECT (batch Eck, s24-16; measured)

| run | total | wall | top set | digits |
|---|---|---|---|---|
| 10¹¹, `ECALC_CKPT_TOP` off | 232.01 s | 290.0 s | — | sha1 578f5efb… ✓ |
| 10¹¹, `=1` | 232.08 s | 290.4 s | 88.89 GB in 44.25 s (2.01 GB/s) in the background; waited 0.00 s for P and for Q | ✓ |
| 10¹¹, `=2` (budgeted) | 241.03 s (bs 101.3: noise, see below) | 299.3 s | 88.89 GB in 44.23 s (2.01 GB/s), not dropped; waits 0.00 s | ✓ |
| 10¹⁰ size 2, none | 113.03 s | — | — | parts' sha1 3fc02547… = `e_1e10.out` ✓ |
| 10¹⁰ size 2, tree sets (`BS_CKPT_MIN_LEVEL=99`) | 110.88 s | — | top tree set 3.60 s, 8.3 GB | ✓ |
| 10¹⁰ size 2, all sets | 122.31 s (bs 29.4 vs 19.4–21.2) | — | 8.3 GB | ✓ |

With O_DIRECT the 10¹¹ top set (89 GB at 10¹¹, not 35.6: the size-1 level-0 set holds P and Q whole) writes at 2.01
GB/s and is completely hidden (0.00 s of wait; `total` equal to the run without it). `=2`'s +9 s is in bs (101.3 vs 94–95 s),
before the set starts: node noise, not the set. At size 2 the tree set costs nothing measurable; all leaf sets +10 s in bs
(the leaf snapshots are synchronous). **Verdict: on a 2 GB/s local disk the top set is free with O_DIRECT; the
question is only the slow disk (W6).**

##### 2.5 Gates, size 4, two real nodes (batches F: job 21518, s24-30, 2026-09-27 10:58–11:37 EDT; W2: job 21526, s24-16 + s24-30, 11:43–11:55 EDT)

- **`./mnaccept.sh $J --only unit,e9`**: **10 passed, 0 failed** (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify,
  t_out, t_mn_grid at 2 procs, e9 identical in both bases) on 9fdcb8d. `t_mul` (default size) VERIFY OK (189 checks).
  (A separate `t_newton` at its default LOGMAX 26 was stopped after 757 s — redundant with mnaccept's `t_newton 20`.)
- **Size 4 on one node** (POOL_LOG=29, as mnaccept's size-4 steps): `MN_OUT_WAVES=2` ASCII: waves 0 and 1 of 2 ranks each
  (each wave wrote in 0.19–0.24 s and waited 0.22–0.26 s for the other), VERIFY OK on all 4, parts `cat | cmp` IDENTICAL;
  `MN_OUT_EARLY=1` packed: VERIFY OK, 4 parts `unpack_digits --cmp` identical; `MN_OUT_WAVES=4` packed: 4 waves of 1,
  VERIFY OK, identical, and **RECHECK OK on 4 packed parts** (the residue form named in the log).
- **Two real nodes** (TCP over 1 GbE, 10⁹, part files on NFS so the parts meet): `MN_OUT_WAVES=2` ASCII: node 0 wrote in
  4.43 s while node 1 waited 4.56 s, then the reverse; VERIFY OK; IDENTICAL. The same packed: 2.05 / 2.42 s; identical.
  `MN_OUT_EARLY=1` packed: "the part file started in the division: its writer ran 5.21 s, joined after 0.00 s"; `dc` 4.5
  → **0.0 s**, total 183.2 → **177.1 s**; identical. (On aac6 the NFS write and the division's TCP exchange share the one
  1 GbE link, so the early writer took 9.5 s instead of 2.0 s; on the target the file system and the fabric are
  separate NICs only if Lustre has its own — it runs over the same Slingshot.) The 10¹⁰ 2-node pair was stopped (a 2-node
  10⁹ takes 180 s on this link; 10¹⁰ would not fit the job); batch W3 runs the pair at 3 × 10⁹.

##### 2.6 Two real nodes at 3 × 10⁹, part files to NFS (batch W3: job 21531, s24-26 + s24-30, 12:33–13:03 EDT; measured, one run each)

| run | `dc` | division's `X Q + window` | total | the writers | parts' sha1 |
|---|---|---|---|---|---|
| n3 (as today) | 12.39 s (12.16 waiting for the writer) | 91.62 s | 557.64 s | 13.64 / 13.74 s | 1f0889f6… |
| e3 `MN_OUT_EARLY=1` | **0.00 s** | 104.51 s (+12.9) | 554.74 s | 55.59 / 56.07 s (ran under the low product) | 1f0889f6… |
| w3 `MN_OUT_WAVES=2` | 26.8 s | 91.90 s | 564.82 s | 13.25 then 13.51 s, each node waiting for the other | 1f0889f6… |

All VERIFY OK, the three runs' parts identical (no reference at 3 × 10⁹; 10⁹ and 10¹⁰ above are against references).
**MN_OUT_EARLY** takes the write off the critical path (`dc` 12.4 → 0) but on aac6 the NFS writes and the division's TCP
exchange share each node's one 1 GbE link, so the low product slowed by 12.9 s and the writer by 4×: net −2.9 s (noise
level). Where storage and fabric do not share a link the gain is the whole `dc`; on the target Lustre runs over the same
Slingshot NICs as SHMEM (8 × 400 Gb/s per node against ≈ 0.6 GB/s of writing: the contention should be small — assumed, not modelled) — measure at TARGET §6 item 5(d). **Waves** at 2 nodes cost exactly one extra write time (+14.4 s): each node has
its own link and the NFS server kept up with both, so capping concurrency only serializes. They pay only where the
aggregate saturates (TARGET §6 item 5(b)).

#### 3. W1 measurements — the writer on aac6's NFS

##### 3.1 From the login node (aac6-login, 10 GbE; `tools/wbench`, 2 GB per run, one run each; 2026-09-27 00:10 EDT) — measured

`/shared` is NFS v3 (`rsize=wsize=65536`, `soft,proto=tcp`). The writer's pattern (256 MB chunks over T pwrite threads):

| mode | threads | chunk | GB/s |
|---|---|---|---|
| O_DIRECT | 1 / 2 / 8 / 32 | 256 MB | 0.74 / 0.86 / 0.74, 0.65, 0.90 / 0.64 |
| O_DIRECT | 8 | 16 MB | 0.68 |
| O_DIRECT | 32 | 1024 MB | 0.96 |
| buffered, no sync | 8 | 256 MB | 1.27 (the page cache: 2 GB fits; not a disk rate) |
| buffered + `fsync` + DONTNEED at close (`sync`) | 8 / 32 | 256 MB | 0.48 / 0.44 |
| buffered + `fdatasync` + DONTNEED per chunk (`drop`) | 8 / 32 | 256 MB | 0.43 / 0.36 |

On this path **O_DIRECT is 1.5–2× faster than the buffered forms that actually reach the server** (0.64–0.96 against
0.36–0.48 GB/s); threads and chunk size move it within the run-to-run noise (±20 %). The Phase 14 reference run's
112 MB/s (T1 841 s for 100 GB) is ≈ 1 GbE line rate — the compute nodes' path, not the mode (Batch B checks with `dd`
on a compute node).

##### 3.2 From a compute node (Batch B: s24-30, job 21473, 2026-09-27 01:10–01:33 EDT) — measured

The compute node's path to `/shared` is **1 GbE** (`enp129s0`, speed 1000). `dd` 4 GiB: O_DIRECT 94.5 / 94.8 MB/s,
buffered + `conv=fsync` 106 / 107 MB/s. `tools/wbench` 1 GB: O_DIRECT 8 / 32 / 2 threads 0.113 / 0.116 / 0.109 GB/s (and
0.116 again), `sync` 0.104, `drop` 0.098 GB/s. Then ecalc at 10¹⁰ (10.00 GB ASCII, 4.44 GB packed), one run each; `total`
= the wall without the write (the run's line), `wall` = the process with the write (both walls, D3); the writer's rate is
bytes / its seconds; Cached / Dirty are the sampler's maxima (the page cache is HBM):

| run | mode | threads, chunk | total (no write) | wall (with write) | writer | GB/s | Cached / Dirty max |
|---|---|---|---|---|---|---|---|
| loc (local `/tmp`, xfs? `local`) | O_DIRECT | 8, 256 MB | 25.75 s | 30.6 s | 4.22 s | 2.37 | 0.6 / 0.0 GB |
| d8 | O_DIRECT | 8, 256 MB | 23.91 | **110.8** | 86.55 | 0.116 | 0.6 / 0.0 |
| d32 | O_DIRECT | 32, 256 MB | 24.63 | 111.4 | 86.29 | 0.116 | 0.6 / 0.0 |
| d32c | O_DIRECT | 32, 1024 MB | 24.22 | 110.8 | 85.88 | 0.116 | 0.7 / 0.0 |
| d2 | O_DIRECT | 2, 256 MB | 25.65 | 122.0 | 95.92 | 0.104 | 0.7 / 0.0 |
| buf | buffered (no sync) | 8, 256 MB | 24.56 | 116.3 | 7.06 + 84.1 in `close` | 0.109 | **10.7 / 7.8** |
| sync | buffered + fsync + DONTNEED | 8, 256 MB | 25.79 | 121.5 | 95.12 | 0.105 | **10.7 / 9.9** |
| sync32 | the same | 32, 256 MB | 25.32 | 120.8 | 95.21 | 0.105 | 10.8 / 8.6 |
| drop | fdatasync + DONTNEED per chunk | 8, 256 MB | 25.27 | 126.2 | 100.56 | 0.099 | 0.9 / 0.3 |
| **pkd** | **packed**, O_DIRECT | 8, 256 MB | 25.20 | **65.3** | 39.30 | 0.113 | 0.8 / 0.0 |
| pks | packed, sync | 8, 256 MB | 23.94 | 67.4 | 42.58 | 0.104 | 5.2 / 3.3 |

Every run VERIFY OK. (The binary was 9654f41 for loc–d32 and 9fdcb8d from d32c on: the difference is W7's NFS rule under
`ECALC_ODIRECT=auto`, not used here.)

**W1 verdict (measured)**: on aac6's NFS the link is the limit (1 GbE ≈ 0.117 GB/s), and **O_DIRECT with 8 or more
threads is the best form: 0.116 GB/s, within 1 % of the link**; the buffered forms are 5–10 % slower and hold up to
10.7 GB of page cache (HBM) with 8–10 GB dirty — the plain buffered form hides the write in `close()` (84 s) rather than
saving it; `drop` keeps the cache small but is the slowest (−15 %). Threads (8 → 32) and the chunk (256 → 1024 MB) change
nothing measurable at this rate; 2 threads lose 10 %. From the login node (10 GbE, §3.1) O_DIRECT is 1.5–2× the buffered
forms. So the Phase 14 default (O_DIRECT) stays right on NFS too, and the reference run's 112 MB/s was the 1 GbE link, not
O_DIRECT. **The packed file cuts the with-write wall 110.8 → 65.3 s (−41 %) at the same rate** (4.44 vs 10.00 GB);
without the write both are ≈ 24–26 s (the packed path's in-run work is no slower: `total` 25.20 vs 23.91–25.79).

#### 4. W5 (c): the top set and the part file within a node

At size > 1 with `ECALC_CKPT_TOP` the top tree set (P's and Q's shares, ≈ 35 GB per node at the target, modelled) is
written by a background thread from the tree's top through the reciprocal and the division; P must be on disk before
`S = P + Q` overwrites it (`newton_mn_pq_hook` stage 0), Q is held past the division if its part is not written yet. The
part file comes after (after T1, or from the low product with `MN_OUT_EARLY=1`). Both go to the node's one Lustre stream.

- **The order is forced by memory, not chosen**: writing the part file first would mean holding P and Q (≈ 2 × 17.5 GB per
  node at the target) past the division, on a node at 434–460 of 480 GB (modelled). Not affordable; no switch built.
- **When the disk is the limit the order does not change the node's end**: the process ends when both are on disk
  (`mn_ckpt_top_finish` joins the set's writer after the output stage). At 0.6 GB/s: 35 GB + 73.8 GB = 181 s of writing
  from the set's start, of which the reciprocal and the division (≈ 60 s, modelled) hide at most 60 s; packed:
  35 + 32.8 GB = 113 s. Without the set: 123 s ASCII, 55 s packed, minus what the low product hides (modelled).
- So the levers are the bytes (packed −41 GB per node; the set dropped −35 GB), the overlap (`MN_OUT_EARLY`), and, across
  nodes, the aggregate (waves, stripes). Within the node nothing else.

#### 5. W6: the top set at the target

**What RECHECK loses without the set** (read from `mn_out_recheck`): with the set, P and Q mod q are recomputed from the
stored limbs and compared with (1) the term recurrence recomputed from scratch and (2) the run's recorded residues
(`<outfile>.t1`). Without it, the run's recorded P, Q residues are used, and they are **still** compared with the
recurrence recomputed from scratch; the digits (read back), the windows, X mod q from the digits and the T1 identity
T(P + Q) ≡ XQ + R are checked exactly as with the set. Lost: check (2) — that the run's residue kernel read the P, Q it
multiplied — and the set's second use, a restart point for the division. `ECALC_CKPT_TOP=2` therefore does not remove
RECHECK; it moves it to the residue form. The plan's wording ("silently removes RECHECK") is corrected: RECHECK now prints
which form it ran (0d67ad0).

**Options weighed**:
- *Packed top set*: rejected — P and Q are already base-10¹⁸ limbs, 8 bytes per 18 digits; there is nothing to pack.
- *Rate-based*: exists (`ECALC_CKPT_TOP=2`, budgeted by the measured write rate, `ECALC_CKPT_TOP_SLACK`).
- *RECHECK from residues only*: exists (above) and is as strong for the digits.

**Proposal (for the user)**: at the target run the headline **without the top set** (the default, `ECALC_CKPT_TOP` unset)
unless a restart point for the division is wanted; RECHECK runs in its residue form. If a restart point is wanted, use
`ECALC_CKPT_TOP=2` so a slow disk drops the set instead of stalling the division: at 0.6 GB/s the 35 GB set needs 58 s
against ≈ 60 s of reciprocal + division (modelled), so it would be dropped or barely finished. The restart it buys is
the tree (init + batch + top + levels ≈ 180 s modelled) at the cost of reading 35 GB back (≈ 45 s at 0.8 GB/s).

#### 6. Open issues

- **Everything about Lustre is unmeasured**: the rate of O_DIRECT vs buffered there (W7's Lustre rule is an assumption),
  the aggregate with 64/576 nodes writing, the OST count, whether `MN_OUT_STRIPE` or waves help. TARGET §6 item 5 (a)–(d)
  is the procedure; `tools/wbench` measures (a) and (b) without a GPU (`srun -N64 tools/wbench -s 8 <dir>/wb.%h`).
- **Corrections still rewrite the file** (size 1 always; size > 1 with `MN_OUT_EARLY=1`): on NFS at 10¹¹ that doubled the
  write (§2.3). Agent K's patch path replaces the redo; `mn_out_early_*` and the `c->early` branch in `out_stage` are
  separate from it (named in §1, W5d).
- `MN_OUT_EARLY` holds ≈ 0.9 GB of host buffers (2 chunks + pinned limbs + the O_DIRECT staging, at 256 MB) during the low
  product; the memory model does not count it (≈ 0.2 % of 480 GB); `MN_OUT_CHUNK_MB=64` quarters it.
- `MN_OUT_WAVES` is ignored with `MN_OUT_EARLY` (its barriers would run on mesh 0 from the background thread during the
  division's exchanges). A wave limit for the early writer would need a separate communicator or a token file.
- The packed file has no per-chunk checksum beyond the part's residues (a flipped bit is found, not located).
- `verify.c` (`tier2_window_offsets`), `newton_db.c` / `newton.h` (the hook) and `ecalc.c` (`mn_early_hook`, the `c->early`
  branch, the hook set) are outside my files: minimal, commented edits.

#### RESUME

- **Done.** Branch `p15-IO` (last commit: see `git log`); aac6 `~/ntt-IO15` at 9fdcb8d (code; later commits are report/docs/
  scripts). All batches read into §2–§3 (A failed harmlessly, W failed at srun — both rerun as E and W2/W3).
- Large files: none left under `~/IO15` (1.5 MB of logs) or on any node's `/tmp` (job 21543 checked s24-16 after the cancelled W2: no `io_*` files).
- For a successor: nothing pending in the task list; the target measurements are TARGET §6 item 5 (a)–(d).

## IO215 — Phase 15 agent IO, second task: per-part conversion of the packed digit files

(former file: IO215.md)

Branch `p15-IO2` from int15b c2aedc2 (packed output is the default there). aac6 clone `~/ntt-IO15` (checked out at
p15-IO2), logs `~/IO15/P/`. Times Eastern. Numbers labelled **measured** / **modelled**.

#### Problem (agent DOC, results/TARGET.md §DOC15)

`tools/unpack_digits` refused a single part ("parts 0..n-1 needed"), so the target's ASCII file could only be made in one
stream over all 576 parts: 42.5 TB through one node, ≈ 20 h at 0.6 GB/s (modelled).

#### Change (`tools/unpack_digits.c`, be2723d; docs dd69fae, df2d8a2)

- Any **consecutive subset** of a run's parts is accepted (one part, or parts k..m; a gap is refused, rc 2). The validation
  that remains: the same run (d, d_out, limb count, pad), consecutive part indices, limb ranges that join, part 0 ending at
  the top limb and the last part reaching limb 0.
- The output is **exactly the subset's byte range** of the ASCII file: "2." only from part 0 (digit 0), the newline only
  from the last part (after digit d_out). Byte range of parts k..m: from 0 (if k = 0) or k0 + 1 (k0 = part k's first
  digit) to k1 + 1 (k1 = the end of part m's digits, cut at d_out + 1), + 1 for the newline if m is the last part. The
  converter prints the range. **`cat` of the per-part outputs in part order is the exact file.**
- The per-part residue check is unchanged (each part's digits against the residues its run stored in the header).
- **`--cmp <ref>`** with a subset seeks the reference to the subset's first byte and compares that range; the "reference
  is longer" check applies only when the subset contains the last part. `digcmp.sh` (whole sets) is unaffected.
- Docs: TARGET §7 (the per-node conversion: `srun -N576 --ntasks-per-node=1` over `$SLURM_PROCID`, then `cat`; the parts are
  on the shared file system, so any node converts any part); the README row of `ECALC_OUT_PACKED`.

#### Tests

- **Host** (`tools/test_unpack.py`, login node and aac6: PASS): for 1–7 parts, each part alone with `-o`, the outputs
  concatenated == the file byte for byte; each part `--cmp`'d alone against the reference; against a reference with one
  changed byte exactly one part differs; parts 1..2 together `--cmp` identical; parts 0 and 2 (not consecutive) refused.
- **Node** (batch P, job 21592, s24-30, 2026-09-27 17:07–17:18 EDT, binary be2723d on int15b; measured):

| test | result |
|---|---|
| size 2, 10⁹ (packed default): each part `-o` alone; `cat` | parts 500000006 + 499999997 bytes; **IDENTICAL** to `ref/e_1000000000.txt` |
| size 2: each part `--cmp` alone | identical to bytes [0, 500000006) and [500000006, 1000000003) of the reference |
| size 4 (POOL_LOG=29), 10⁹: each part alone; `cat` | 250000004 + 250000002 + 250000002 + 249999995 bytes; **IDENTICAL** |
| size 4: each part `--cmp` alone | identical to [0, 250000004), [250000004, 500000006), [500000006, 750000008), [750000008, 1000000003) |
| size 2, 10¹⁰: part 0 alone, then both parts in parallel on the node; `cat \| sha1sum` | 3fc02547… = `e_1e10.out`'s; alone 8.46 s, both in parallel 10.25 s |
| size 1, 10¹¹: the run | VERIFY OK; total 196.88 s (the new defaults), wall 198.6 s; 44.44 GB written in 25.25 s |
| size 1, 10¹¹: `unpack_digits f \| sha1sum` | **578f5efb…** = `e_1e11.out`'s (`~/V214/e_1e11.sha1`); 72.6 s |
| size 1, 10¹¹: `unpack_digits -o f.txt f` | 173.4 s |

#### Per-part conversion rate (measured, local NVMe on s24-30)

| case | packed read | ASCII out | wall | rate (ASCII bytes / wall) |
|---|---|---|---|---|
| one 5 GB part (10¹⁰ / 2), to a file | 2.22 GB in 1.4 s | 5.00 GB, writer 3.5 s | 8.46 s (incl. the final fsync) | 0.59 GB/s |
| two such parts at once on one node | 1.7–1.8 s each | 3.5–3.6 s each | 10.25 s | 0.98 GB/s together |
| 10¹¹, to a pipe (`\| sha1sum`) | 44.44 GB in 26.7 s (1.67 GB/s) | 100 GB, 72.3 s | 72.6 s | 1.38 GB/s (sha1sum's rate) |
| 10¹¹, to a file | 44.44 GB in 32.5 s | 100 GB, 144.9 s | 173.4 s | 0.58 GB/s |

The converter's own work is small (format 2.5 s and residues 4.4 s per 10¹¹ digits, OpenMP); the rate is the disk's
write rate. **At the target (modelled)**: each node converts its 73.8 GB part at the Lustre write rate, 0.6–0.8 GB/s →
**92–123 s per node, all 576 in parallel** (≈ 2 min instead of ≈ 20 h), if the file system's aggregate carries 576
writers; otherwise 42.5 TB at the aggregate rate (TARGET §6 item 5(b)). A per-part `--cmp` reads 32.8 + 73.8 GB per node
(≈ 2–2.5 min at 0.8 GB/s, modelled).

#### Open issues

- The aggregate Lustre rate for 576 concurrent converters is unmeasured (the same question as the run's own write).
- A subset that holds only digits past d_out (impossible with the code's shares, which are ≥ 1 limb of real digits) would
  produce an empty range; handled (0 bytes), not tested on a node.

#### RESUME

Done. p15-IO2 at the last commit of this report; nothing running; no files left under `~/IO15` or in s24-30's `/tmp`.

## KP15 — the correction patch on packed output (a defect on main, B2 44c1642)

(former file: KP15.md)

Agent KP, branch `p15-KP` from main 44c1642 (B2). aac6 clone `~/ntt-KP15`, scratch `~/kp15tmp`. Times Eastern (aac6's Central + 1 h).

#### 1. The defect (reproduced, measured)

Two defaults of 2026-09-27 did not compose: `ECALC_CORR_PATCH=2` (K) patches the digit file's tail by writing **ASCII bytes at
their ASCII offsets** (`mn_out_tail_core`'s byte image `pos(k) = (k ? k + 1 : 0) − fbase`), while `ECALC_OUT_PACKED=1` (IO) writes
a 4096-byte header and base-10¹⁸ limbs (8 bytes per 18 digits, most significant first). When a correction comes after the
writer started (size 1: always; size > 1: with `MN_OUT_EARLY=1`), the patch aimed past the packed file's end, `patch_bytes`
refused it, and the run ended **VERIFY FAILED** with the file holding the **uncorrected** digits.

Reading the code found a second, quieter part of the same defect: **the packed writer ignored `t2_defer`** (the ASCII branch of
`out_run_one` passes `d_out + 1 − t2_defer` to `tier2_range`; the packed branch passed `d_out + 1`). So the windows in the
deferred zone were checked by the writer on the uncorrected digits *and* again by the patch. On e's windows at these sizes it did
not fire, but `t_patch` shows it: "T2 window@9 (file) BAD" on the packed form.

Why the regressions missed it: 10¹¹ on the defaults happened to need no correction that reached the file, and the
forced-correction runs (`ECALC_TEST_CORR=7` at 10⁸ / 10⁹) change only digits past d_out, so the ASCII patch wrote 0 bytes and
never touched the file. `tests/t_patch` was not in `mnaccept`'s unit step; run on main it fails on the packed default.

**Reproduction with the base binary (44c1642), stage A, job 21771 on s24-16, 2026-09-28 19:12–19:18 EDT** (measured; "digits"
= `digcmp.sh` against the first d + 2 bytes of `ref/e_1000000000.txt` + a newline; "residues" = `tools/unpack_digits -n`):

| run | env | rc | VERIFY | digits | the log |
|---|---|---|---|---|---|
| 511461828, size 1 | `ECALC_TEST_CORR=14` (defaults) | 1 | VERIFY FAILED | DIFFERS (residues ok: the file is the uncorrected X, self-consistent) | "mn_out: patch: …base_c511_1.txt holds 227320472 bytes, the patch ends at 511461830"; "patch: X +14 … FAILED" |
| 511461828, size 1 | the same + `ECALC_OUT_PACKED=0` | 0 | VERIFY OK | identical | 11 bytes patched |
| 820719000, size 1 | `ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-44` | 1 | VERIFY FAILED | DIFFERS | "holds 364768104 bytes, the patch ends at 820719002" |
| 108388422, size 2 | `ECALC_TEST_CORR=36` (early writer, default) | 1 | all 2 nodes: VERIFY FAILED | DIFFERS | node 0: "…part0001 holds 24090416 bytes, the patch ends at 54194220" |
| 108388422, size 4 | `ECALC_TEST_CORR=36` | 1 | all 4 nodes: VERIFY FAILED | DIFFERS | node 0: "…part0003 holds 12047256 bytes, the patch ends at 27097110" |
| 10⁹, size 1 | `ECALC_TEST_CORR=7` | 0 | VERIFY OK | identical | (the change is past d_out: 0 bytes; the packed file's lowest limb stays the uncorrected one) |
| `tests/t_patch` (login node) | defaults (packed) | 1 | VERIFY FAILED | — | 2258 of 5165 checks failed |
| `tests/t_patch` | `ECALC_OUT_PACKED=0` | 0 | VERIFY OK | — | 5165 checks |

These match DKM's controls (job 21768).

#### 2. The fix: a packed-form patch (built; not the guard)

I built the proper patch, not the guard. The limb image is simpler than the ASCII one: a correction of X by dx changes exactly
the global limbs [0, m) that its carry / borrow reaches, and a part's limbs are one contiguous byte range. The rest of K's
machinery (the low limbs gathered, more while the carry goes on; the old / new digits; the residue adjustment; the deferred
T2 windows; last / tail / first) is unchanged and independent of the file form. Functions touched, all in `ecalc/mn_out.c`:

- **`packed_patch`** (new): reads the part's header and requires this run's complete part (magic, version, `complete = 1`, d, d_out,
  k0, k1, lo ≤ hi ≤ nl, nq). The changed limbs of this part, [lo, min(hi, m)), are at the bytes ECP_HDR_BYTES + 8 (hi − 1 − i). They
  go through `patch_bytes` (checked to hold the old limbs, rewritten in whole 4 KiB blocks with O_DIRECT when the writer used it,
  fsynced, the file cut back to size, read back and checked). Then **the header's residues**: the 64 bytes of `dres` are checked
  to be the run's (`o->dres`, which `mn_out_finish` wrote), moved by Σ (new_i − old_i) · B^(i − lo) mod q, rewritten and read back.
  All the changed limbs are patched, including those that hold only digits past d_out, because the packed file holds X's limbs
  whole.
- **`mn_out_tail_core`**: it keeps the corrected low limbs (`nw`, which it used to free early) and calls `packed_patch` when
  `o->packed`. The ASCII branch is K's code, unchanged.
- **`patch_bytes`**: the O_DIRECT decision is now `out_mode(name) == OUT_DIRECT`, the writer's own rule. Before, it was
  `sp_odirect() && ECALC_OUT_ODIRECT`, the pre-IO rule, which differs from the writer when `ECALC_OUT_MODE` or
  `ECALC_ODIRECT=auto` is set. On the defaults both rules give the same answer.
- **`out_run_one`**, packed branch (one line): the window limit is `d_out + 1 − min(t2_defer, d_out + 1)`, as in the ASCII
  branch, so the deferred windows are left to the patch.

The in-run checks on the new digits are unchanged from K's code: `digits == X mod q` is adjusted by new − old
(`f->dres_adj`, whole string), and the deferred T2 windows are checked on the new digits. The sidecar gets the corrected
residues and tail, so `ECALC_RECHECK` works on the patched parts. `recheck_*` itself needed no change, because it reads the limbs.

Cost: one 4 KiB block of limbs plus one header block per affected part. Measured: 8 bytes rewritten per run in stage A, patch
time ≤ 0.003 s as printed.

Other edits: `ecalc/packed_fmt.h` has a comment on the patched residues. `ecalc/README.md` updates the `ECALC_CORR_PATCH` row
and the mnaccept section. `ecalc/mnaccept.sh` gets the `corr` step and `t_patch` in `unit`. `tests/t_patch.c` and
`tools/test_unpack.py` are extended. New `archive:drivers/kp15_batch.sh` holds the node batches.

#### 3. Tests

##### 3.1 Host tests (aac6 login node, `/shared` NFS, commit 4a64fb4 = the code of the branch)

- **`tests/t_patch`** (extended). Without `ECALC_OUT_PACKED` in the environment it runs itself twice, `=0` (ASCII: K's checks) and
  `=1` (packed). The packed checks: every part's limbs equal the corrected X's limbs [lo, hi) in file order, with no extra bytes;
  the header is complete and its `dres` equals those limbs mod each q (computed independently by `vf_limbs_mod`);
  `tools/unpack_digits --cmp` against the corrected ASCII file passes, which includes its residue check. In both forms, a second
  patch of a patched part must be refused exactly when the first one changed bytes. The cases are 80 random X up to 6000 digits,
  1–5 parts, chunks of 1–9 limbs, T2 zones 0 / 30 / 100 / 4096, and chains up to 5976 digits (69 over one limb, across parts).
  **Measured: `ECALC_ODIRECT=1` and `=0` both VERIFY OK (both forms): ASCII 6365 checks, 0 failures (716 second patches
  refused); packed 6365 checks, 0 failures, 1200 packed parts checked, 761 second patches refused.** strace, packed form:
  2283 `O_RDWR|O_DIRECT` opens with `ECALC_ODIRECT=1`, 0 with `=0` (2284 plain `O_RDWR`), so both patch paths ran.
- **`tools/test_unpack.py`** (extended with `patched_cases`). Parts are written from an X ending in a run of 9s / 0s. A
  Python patcher, independent of the C, rewrites the changed limbs and the header residues. Cases: d 36–36000, 1–5 parts,
  dx ∈ {+7, −3, +1, −60, +9}, and chains of up to 1201 limbs across parts. Checks: `unpack_digits --cmp` passes (residue check
  and ASCII equal to the corrected digits). With the limbs patched but not the header, the residue check fails (rc 1) and
  `--no-res --cmp` passes. **Measured: PASS** (the 8 original cases + 5 patched), on the login node and locally.

##### 3.2 Forced corrections with the fix (stage A, job 21771 on s24-16, 19:12–19:18 EDT, measured)

Every run below: rc 0, VERIFY OK on every node, digits **identical** (against the reference prefix, or `ref/e_1000000000.txt` at 10⁹),
`unpack_digits -n` residues ok, and the patch rewrote **8 bytes (one limb) in 1 part file** (node 0's at size > 1).

| run | size | env (on the defaults: packed, CORR_PATCH=2, MN_OUT_EARLY=1) | digits changed | also |
|---|---|---|---|---|
| 511461828 | 1 | `ECALC_TEST_CORR=14` | [511461818, 511461828] | `ECALC_RECHECK=1`: RECHECK OK |
| 511461828 | 1 | + `ECALC_ODIRECT=0` | the same | |
| 511461828 | 1 | + `ECALC_OUT_PACKED=0` (the ASCII path) | the same | 11 bytes, as K's |
| 820719000 | 1 | `ECALC_CORR_PATCH=1 ECALC_TEST_CORR=-44` | [820718991, 820719000] | |
| 820719000 | 1 | `ECALC_TEST_CORR=-44` | the same | RECHECK OK |
| 10⁹ | 1 | `ECALC_TEST_CORR=7` / `-7` | [1000000007, 1000000008] (past d_out: the lowest limb) | +7: RECHECK OK |
| 108388422 | 2, 3, 4 | `ECALC_TEST_CORR=36` | [108388413, 108388422] | size 2: 3 RECHECK OK (2 nodes + all) |
| 108388422 | 2, 4 | + `MN_OUT_EARLY=0` | the same | |
| 108388422 | 2 | + `ECALC_ODIRECT=0` | the same | |
| 108072396 | 2, 4 | `ECALC_TEST_CORR=-39` | [108072388, 108072396] | size 4: 5 RECHECK OK |
| 108072396 | 2 | + `MN_OUT_EARLY=0` | the same | |
| 10⁹ | 4 | `ECALC_TEST_CORR=7` | [1000000007, 1000000008] | |

Run times 10–18 s each (measured, wall around srun).

##### 3.3 mnaccept with the corr step (stage B, job 21772 on s24-16, 2026-09-28 19:18–19:43 EDT, measured)

`./mnaccept.sh 21772 --only unit,e9,mn,recheck,corr` on commit 99fd5f3 (the code of the branch) gave **21 passed, 0 failed (1466 s)**:
- unit: t_ntt 24 (3215 checks), t_mul 20 (189), t_bs (10), t_dbig 0 (555), t_newton 20 (620), t_verify (334), t_out, **t_patch (both
  forms, ≈ 100 s)**, t_mn_grid at 2 procs (200).
- e9 size 1, LIMB_BASE=10 identical (total 10.69 s) and LIMB_BASE=2 identical (20.81 s).
- mn e8 sizes 2, 3, 4 and e9 sizes 2, 4 identical, with all nodes VERIFY OK.
- recheck e9 size 1 and e8 size 2: RECHECK OK with P, Q from the checkpoint, and RECHECK FAILED on the corrupted copy.
- **corr** (new): 511461828 size 1 with `ECALC_TEST_CORR=14` is identical, the patch wrote 8 bytes in 1 part file, and
  `ECALC_RECHECK=1` on the patched file gave RECHECK OK. 108388422 at sizes 2 and 4 with `ECALC_TEST_CORR=36` is identical, with
  8 bytes patched.

**Negative control (stage D, job 21779 on s24-16, 20:09–20:10 EDT, measured):** the same `corr` step run on the base binary
(44c1642, in a scratch worktree of the clone) gave **0 passed, 3 failed** (58 s). Each failure shows rc 1, DIFFERS and "holds
227320472 / 24090416 / 12047256 bytes, the patch ends at …". So the step catches this defect.

##### 3.4 The standard sizes on the defaults (stage C, job 21775 on s24-16, 19:43–20:09 EDT, measured)

| size | rc | VERIFY | digits | total | corrections |
|---|---|---|---|---|---|
| 4 × 10¹⁰, size 1 | 0 | VERIFY OK | **identical** to `~/ntt/ecalc/results/e_4e10.out` (`digcmp.sh`: `unpack_digits --cmp`, the reference evicted first) | 61.23 s | 0 ("patch: X +0 … nothing to patch") |
| 10¹¹, size 1 | 0 | VERIFY OK | **identical** to `~/ntt/ecalc/results/e_1e11.out` | 215.26 s | 0 |

Walls with the compare over NFS: 434 s and 1128 s. Neither run needed a correction, so neither exercised the patch. The patch
is covered by §3.1–§3.3.

#### 4. Open issues

- **End-to-end chains are one limb long.** At these sizes e's digits give chains of ≤ 11 digits, inside one limb, so every
  node run patched a single limb in node 0's part. Chains across limbs, chunks and parts, up to 5976 digits and 1201 limbs,
  are covered by `t_patch` and `test_unpack.py` only.
- **A crash between the limb write and the header write** leaves a part whose limbs are corrected but whose header residues
  are not. This cannot pass silently: `unpack_digits` (and `test_unpack.py`'s header-not-patched case) then fails the residue
  check, rc 1. The run itself would not reach VERIFY OK.
- **`patch_bytes` now follows `out_mode`** (§2) in both forms. On the defaults the O_DIRECT decision is the same as before. Not
  run: `ECALC_OUT_MODE=direct|buffered|sync|drop` with a forced correction.
- Not run: mnaccept `ckpt`, `full` and `stress`. The 4 × 10¹⁰ and 10¹¹ runs of §3.4 cover the standard size. No timing study was
  done: the patch is two 4 KiB blocks per affected part, ≤ 0.003 s as printed.
- Untracked files `ecalc/slurm-2177{1,2,5}.out` are in `~/ntt-KP15` (my clone, not `~/ntt`).

#### RESUME

- 2026-09-28 18:50 EDT: branch made; code and tests written and committed (4a64fb4, 99fd5f3, e77cac8). Host tests pass (§3.1).
- 19:12–19:18 EDT: stage A (job 21771) done: the reproduction and the fix's forced corrections (§1, §3.2), all as expected.
- 19:18–19:43 stage B (21772): mnaccept 21 passed, 0 failed. 19:43–20:09 stage C (21775): 4e10 and 1e11 identical. 20:09–20:10
  stage D (21779): the corr step fails on the base binary, as it should. No job of KP is left; the scratch worktree is removed.
- **DONE.** For the integrator: merge `p15-KP`; the fix is on the default path (packed output), and there is no new switch.

## S114 — S1 (Phase 14, W1): spill and I/O — E1, E3, E4, E12 of internal apumult study

(former file: S114.md)

Branch `p14-S1` from main @ ae1d872; aac6 clone `~/ntt-S114`. Every new behavior is behind a switch, off by default;
with every switch off the code paths are the old ones (the `FILE *` checkpoint I/O and the buffered output writer run
unchanged; the only always-on change is that the background writer's and the fallback checkpoint buffers come from
`posix_memalign` instead of `malloc`).

#### RESUME (updated 00:55 EDT)

- **Done (code, builds clean on aac6's login node, no warnings)**: `ecalc/spill.c/.h` (E3's `sp_file`, E4's spill
  primitive), `ecalc/memsample.c/.h` (E1's `live:` line, E12's sampler), the live lines hooked in, `ECALC_ODIRECT` in the
  checkpoint writer / output writer / recheck read / binary output, `mnaccept.sh`'s `cmpref` with `dd iflag=direct`,
  `tests/t_spill.c`, README switch rows, `archive:drivers/ecalc/s14_batch.sh` (the node batch).
- **Tested**: the `sp_file` carry logic on the login node (host memory; O_DIRECT and buffered; `/tmp` and the shared
  home both accept O_DIRECT): 38 809 657 bytes written in odd pieces, read back in other odd pieces, identical, file
  size exact (measured). `cmpref_direct` checked locally (identical / DIFFERS on a truncated copy).
- **Node batch done (job 21130, 00:04–00:32 CDT)**: t_spill VERIFY OK at 1, 8 and 32 GiB. The 4e10 pair is identical to
  the reference both ways, and O_DIRECT cut the elapsed time from 154 s to 82 s (tables below). mnaccept unit,e9,recheck
  under ECALC_ODIRECT=1: see the Tests table.
- **Next step**: read `ecalc/results/S114_batch/summary.txt` on aac6 (`~/ntt-S114/ecalc`), fill the tables below;
  W2: measure E3 (the checkpoint rate before / after) and E4 (the primitive's rate), E12 in use; L1 wires the primitive
  into E5 / E6.

#### Design

##### E1 — the live-bytes column (`memsample.c` `mem_live_line`, `ECALC_LIVE=1` or `ECALC_VERBOSE=2`)
One `live:` line per bs level (`binsplit.c` level loop), per tree level at size > 1 (`mn.c mn_tree`), per Newton
doubling (`newton_db.c recip_db2` and `recip_mn`) and at the division's end (`divmod(dev)`, `divmod(mn)`): per APU the
block pool's live bytes now, the **peak since the previous line** (a window counter `g_win_peak` in `dbig.c`, updated
in `live_add` under the pool mutex, reset by the line), the pool's size (live + free extents), the peak's share of the
pool, and `/proc/meminfo` MemAvailable / Cached. Accessors added to `dbig.c/.h` (minimal, commented):
`db_pool_live(dev)`, `db_pool_window_peak(dev, reset)`. The leaf levels live in the region pools (the level line's
`pool` column), so their block-pool live bytes are ~0; the device-tier levels, the tree and the reciprocal are what the
column measures.

##### E3 — nothing written stays in the page cache (`ECALC_ODIRECT=1`)
`sp_file` (`spill.c`): a sequential file through 4 KiB-aligned buffers. A write appends into the buffer after the
carry (< 4 KiB kept from the last call), writes the whole blocks with O_DIRECT, keeps the rest; `spf_close` writes the
last block padded and `ftruncate`s the file to its exact size, then `fsync`. A read `pread`s the aligned blocks that
cover the range and DMAs from the offset inside. Device memory goes through the caller's pinned buffers with
`hipMemcpyAsync` on the file's own stream per device (never the pipeline's streams), double-buffered when two buffers
are given (the next DMA under this `pwrite`; on read the next `pread` under this DMA). Host memory: `memcpy`. If the file
system refuses O_DIRECT (EINVAL, e.g. tmpfs) it falls back to buffered + `fsync` + `posix_fadvise(DONTNEED)` at close.
The public calls keep the calling thread's current device.

Where it is used under `ECALC_ODIRECT=1`:
- **the checkpoint writer** (`binsplit.c`): the level sets (`ckpt_region_io`, `ckpt_devnodes_io`), the tree sets and
  the top set (`tree_io`, synchronous and the background writer `bs_ckpt_bg_*`), writes and restart reads. The data
  files go through a `ckf` (the old `FILE *` or an `sp_file`); the synchronous paths split the 1 GiB DMA buffer into two
  halves (double-buffered); the background writer stays single-buffered, one DMA per run, with its cancel contract
  unchanged: `spf_write_dev`'s guard holds `dma_active` only around the device read and checks `cancel` before each.
  The small headers stay stdio, `fsync`'d, then dropped from the cache (`sp_drop_cache_path`).
- **the output file** (`mn_out.c` writer thread): O_DIRECT (`ECALC_OUT_ODIRECT=0` keeps it buffered). The chunks arrive
  in file order; each is copied (8 threads) into an aligned staging buffer after the carry, whole blocks written
  (8 threads), the rest carried; `mn_out_finish` writes the padded last block, truncates, `fsync`s. The binary-base
  output (`ecalc.c`) gets `fsync` + DONTNEED; the recheck's digit read (`mn_out.c`) DONTNEED after it.
- **the reference comparison** (`mnaccept.sh cmpref`): with `ECALC_ODIRECT=1` in the environment, both sides are read
  with `dd iflag=direct` into `cmp` through process substitution, and no `.all` copy is made.

Trade-off to measure (W1 batch / W2): buffered output lands in the page cache at memory speed and is written back
after the run (leaving up to 40 GB dirty, the "page-cache eviction rule"); O_DIRECT writes at the disk's rate
(2.0 GB/s measured by agent M: 20 s for 40 GB) inside the output stage. The checkpoint side should only gain (0.3–0.6
→ ≈ 2 GB/s expected, apumult_M).

##### E4 — the spill primitive (`spill.h`)
```
spill *spill_start(dbig *x, size_t lo, size_t hi, const char *name, int flags);   /* ECALC_SPILL_DIR; 0 = disabled */
spill *spill_start_dir(dbig *x, size_t lo, size_t hi, const char *dir, const char *name, int flags);
int    spill_done(spill *s);      /* non-blocking */
int    spill_wait(spill *s);      /* completion; SPILL_FREE: the blocks returned to the pool here */
int    spill_restore(spill *s, dbig *x);   /* db_reserve(x, cap) if needed + read back [lo, hi); x->n = spilled n */
void   spill_drop(spill *s);      /* unlink the files, free the handle (and the blocks if never restored) */
void   spill_prealloc(void);      /* the bounce buffers now, from the main thread */
```
Limbs [lo, hi) are split by quarter; four threads, thread d writes quarter d's part from APU d through its own two
bounce buffers (`mem_hstage_alloc(d, SPILL_CHUNK_MB)`: pinned, first-touched on APU d's NUMA node; 2 x 256 MiB per APU
by default) with O_DIRECT into `<dir>/<name>.<pid>.<seq>.q<d>`. Background: `spill_start` returns at once.
`SPILL_FREE` takes the descriptor (the caller's is zeroed) and frees the blocks **in `spill_wait`** (the caller's
thread, so the pool's layout stays deterministic — a free from the writer thread would make the pool's extents depend
on timing). The restore reads file d into the target's quarters (the target's `qc` may differ: runs split by quarter),
four threads. A per-APU mutex serializes two spills on one APU's bounce.

##### E12 — the memory sampler (`ECALC_MEM_SAMPLE=<s>`, `ECALC_MEM_SAMPLE_FILE`)
A thread started in `ecalc.c` before `rns_init` (so init is sampled), one line per period: RSS, MemFree, MemAvailable,
Cached, Dirty, Writeback (`/proc/meminfo`, which sees the page cache; `hipMemGetInfo` does not) and the pool's live
bytes per APU; a summary at exit (`atexit`): min MemAvailable, max Cached / Dirty / RSS / live per APU. The rank in
each line is `COMM_RANK` (set before `mn_init`) or `mn_rank()`.

#### Files touched
Mine: `ecalc/spill.c`, `spill.h`, `memsample.c`, `memsample.h`, `tests/t_spill.c`, `s14_batch.sh`; `binsplit.c`
(checkpoint I/O only: `ckpt_buf`, `ckf`, `ckpt_open/close/abandon`, `ckpt_region_io`, `ckpt_dbig_io`, `tree_io`'s cancel
line, `ckpt_finish`'s cache drop; plus the one `live:` line in the level loop); `mn_out.c` (the writer's file I/O, the
recheck read's DONTNEED); `ecalc.c` (the sampler start, the binary output's drop); `mnaccept.sh` (`cmpref`); Makefile;
README (switch rows).
Outside my list, minimal and commented: `dbig.c/.h` (two accessors and the window peak), `newton_db.c` (four
`mem_live_line` calls, one include — no logic touched), `mn.c` (one `mem_live_line` call, one include).

#### Tests
| test | command | size | result | time |
|---|---|---|---|---|
| sp_file carry, host memory, login node | `/tmp/t_spf_host_s1 /tmp`, `... ~` (scratch test linking spill.o, mem.o) | 38.8 MB, odd pieces | identical, exact size, O_DIRECT granted on both fs (measured) | < 1 s |
| cmpref_direct logic | local bash, 2 part files / 1 file / a truncated copy | 10 KB | identical / identical / DIFFERS | — |
| **t_spill** (node s24-30, job 21130) | `tests/t_spill /tmp 1 8 32` | 1, 8, 32 GiB | **VERIFY OK (70 checks)**; all 8 sp_file modes identical | 2 min 9 s |
| **4e10, ECALC_ODIRECT=1 ECALC_CKPT_TOP=1** | `s14_batch.sh` step 2 | 4 x 10^10 | **digits identical** to e_4e10.out (dd iflag=direct compare) | 82 s elapsed |
| **4e10, ECALC_CKPT_TOP=1 (buffered)** | `s14_batch.sh` step 3 | 4 x 10^10 | **digits identical** | 154 s elapsed |

##### t_spill rates (measured, aac6 s24-30, `/tmp` = the Samsung 980 PRO, four APUs in parallel, 2 x 256 MiB bounce per APU)
| size | A: kept, write / read GB/s | B: SPILL_FREE (async restore) | C: partial range, O_DIRECT | C: buffered + fsync + DONTNEED | Cached before -> after |
|---|---|---|---|---|---|
| 1 GiB | 3.00 / 3.37 | 2.95 / 3.42; pool live 1.00 -> 0.00 GiB | 2.93 / 3.39 | 1.32 / 2.62 | 0.49 -> 0.49 GiB |
| 8 GiB | 2.23 / 3.51 | 2.00 / 3.52; live 8.00 -> 0.00 | 1.98 / 3.51 | 1.31 / 2.72 | 0.49 -> 0.50 |
| 32 GiB | **2.05 / 3.53** (16.7 s / 9.7 s) | 2.02 / 3.53; live 32.00 -> 0.00 | 2.03 / 3.52 | 1.32 / 2.73 | 0.50 -> 0.50 |

The write matches agent M's single-APU bounce rate (2.0–2.2 GB/s, the drive's sustained O_DIRECT write). The 1 GiB
write at 3.0 GB/s is the drive's fast cache (SLC). The read, 3.5 GB/s, is below M's 5.0 GB/s for four APUs sharing the
disk. Possible causes (not yet measured): a 256 MiB chunk against M's 1 GiB, or four files read at once. **Try
`SPILL_CHUNK_MB=1024` in W2.** Pinning the bounce took 0.31 s. The page cache did not grow in any test, and the
buffered fallback's DONTNEED also left nothing in it.

##### E3: 4e10 with the top-level checkpoint on, O_DIRECT against buffered (measured, same node, back to back, the reference evicted first)
| | ECALC_ODIRECT=1 | buffered (the old code) |
|---|---|---|
| wall, `total` line (bs + dm + init; the output stage joined after it) | 65.02 s | 65.20 s |
| elapsed, process start to exit (with the output file and the top set) | **82 s** | **154 s** |
| top set, 35.56 GB, background | **16.78 s, data at 2.12 GB/s**, fsync + header 0.03 s, 0 s waited | 111.31 s: data 41.8 s at 0.85 GB/s, then **fsync 69.6 s**; 4.9 s waited at the join |
| output file, 40 GB (the writer thread) | **20.7 s** (1.93 GB/s); dc waited 15.7 s for the writer | 89.0 s; dc waited 83.9 s for the writer |
| page cache (sampler, every 2 s) | Cached max **0.56 GB**, Dirty max 0.01 | Cached max **76.2 GB**, Dirty max 28.9 GB |
| digits | identical | identical |

So O_DIRECT **halves the run's elapsed time with the top set on (154 -> 82 s)** and leaves nothing in the page cache.
In the buffered run, the output and the top set write back through ext4 together. The 76 GB they leave cached (28.9 GB
of it dirty) is what forced the eviction rule on the runs that follow. The computed phases are unchanged: bs 24.7
against 24.8 s, dm 23.5 against 24.0 s, the reciprocal 10.3 against 10.6 s. **Adoption proposal:** `ECALC_ODIRECT=1`
as the default whenever a file is written. Even without the top set, the output alone is 20.7 s against the page
cache's absorb-then-writeback. The user decides.

##### E1: the live column at 4e10 (measured, the O_DIRECT run's `live:` lines, per APU, the four APUs identical)
| window | pool live at the line | window peak | pool (live + free) | peak / pool |
|---|---|---|---|---|
| bs levels 1–22 (batch tier: the region pools, not the block pool) | 0 | 0 | 0 | — |
| bs level 23 mdev | 8.89 GB | 8.89 | 33.07 | 26.9 % |
| bs level 24 mdev | 8.89 | 20.55 | 33.07 | 62.1 % |
| bs level 25 mdev (top) | 8.89 | 19.30 | 33.07 | 58.4 % |
| reciprocal, every doubling to j = 1.1e9 | 26.67 | 26.67 | 33.07 | 80.6 % |
| reciprocal, the last doubling (j 1.1e9 -> 2.2e9) | 26.67 | **29.78** | 33.07 | **90.1 %** |
| division (to its end) | 17.78 | 26.67 | 33.07 | 80.6 % |

The reciprocal's reservations (r, r2, t1 at final capacity, 26.67 GB per APU) are live from the first doubling. Their
peak use, 29.78 GB, comes only in the last doubling. That constant 26.67 GB plateau is what E2 (L1's `DM_TIGHT`)
removes. The 3.3 GB per APU never used (33.07 − 29.78) is the pool's slack at 4e10.

#### Node batch (`archive:drivers/ecalc/s14_batch.sh`)
Launched on aac6's login node (`setsid nohup`), one 30-min job, cancelled if not started by 03:58 EDT, every step
skipped when it cannot end by 04:27 EDT; the job cancels itself at the end (trap). Output:
`~/ntt-S114/ecalc/results/S114_batch/` (summary.txt, t_spill.log, e4e10_{odirect,buffered}.log, mem_*.txt,
mnaccept.log).

#### Open issues
- `mem.c`'s registry (`mem_is_registered`) is read without its lock; `spill_prealloc` from the main thread avoids a
  realloc under a reader when the spill is used inside a phase (W2, L1's E5).
- The bounce buffers cost 2 GiB of host memory (HBM) at the default chunk; the formula (`dm_layout`) will need them if
  E5 spills inside the dm phase (L1).
- A restore is synchronous (four threads); an asynchronous restore (read P back under the last reciprocal products)
  can be added the same way as the background spill if E5 needs it.
