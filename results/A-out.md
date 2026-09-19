# A-out — per-node output streamed in chunks, rank-local T1/T2 (Phase 9, PLAN.md §19: M5 + C1)

Branch `a-out` (from `main` @ a75474d). Owned: `ecalc/verify.c/.h`, new `ecalc/mn_out.c/.h`, the
driver's output tail in `ecalc/ecalc.c` (from the point where X is on the host to the end); plus
`ecalc/Makefile` (the new object and test) and the new `ecalc/tests/t_out.c`.

## What it does

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
plugs in: `src.dev = &Xm.sh` with `mdb_share` for lo/cnt (the scatter goes), and the P, Q, R residues
per share by `db_mod_qs` + `mn_out_res_combine` instead of the broadcast.

## Tests

Job 20726 (one node, ppac-pl1-s24-26), clone `~/ntt-aout` @ b432d9e, script `ecalc/aout_test.sh`
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

## Open issues

- **The file write on aac6 cannot hide.** 40 GB at 0.5–1.4 GB/s is 30–80 s whatever the target
  (NVMe page cache throttled, or NFS), against a ≈ 10 s low product. The streamed writer bounds the
  memory (two 256 MB chunks + a 128 MB limb buffer) so it cannot run ahead of the disk; `total`
  therefore stops before the join, as the old `fwrite` did, and the dc line reports the wait
  ("waiting for the writer 72.5 s"). On the target machine's parallel file system the write should
  hide under the low product; on aac6 the run's wall clock including the write is ≈ 175 s with a
  local disk (vs. an unmeasured but comparable `fwrite` + `close` before). If a 4 × 10¹⁰ run's
  file is not needed, omit `<outfile>` (nothing is written; the checks still run).
- Two or three **real nodes** were never idle together during the session (all three nodes were
  held by the other agents' jobs); the multi-node paths were exercised with 2–4 node-processes
  on one node over the same TCP meshes. To run when free: `SLURM_JOB_ID=$J ./mnrun.sh 2 env
  POOL_LOG=29 ./ecalc 1000000000 ~/aout/e.txt` on a 2-node allocation, then `cat` the parts.
- **The A-div hooks** (`out_stage`, two `HOOK A-div` comments): X's share from `Xm.sh` with
  `mdb_share` instead of the scatter; P, Q, R residues per share by `db_mod_qs` + `mn_out_res_combine`
  instead of node 0's broadcast. The stand-in scatter (`mn_out_scatter_standin`) can then go.
  Note the size-1 flow keeps X on the host (17.8 GB): with A-div's X on the device the size-1 writer
  can use the device source too (`src.dev`, no host X at all) — a 17.8 GB further drop.
- `mn_out_allgather_u64` allocates its two small device buffers per call (a few calls per run).
- Part naming is file order (`part0000` = the head), i.e. k = size − 1 − rank, since node 0 holds
  the low limbs of an `mdb`; if the integrator prefers `.part<rank>` it is one line in `mn_out_run`.
- `ECALC_OVERLAP=0` at size 1 formats and writes after `total` (synchronously); before, dc and T2
  were inside the timed phases on that path.

## Files touched outside my list

`ecalc/Makefile` (the `mn_out.o` object, `tests/t_out`), new `ecalc/tests/t_out.c`, and inside
`ecalc.c` above the output tail: the `pq_bg` struct gained the term range `[a0, b1)`, `struct x_bg`
and `struct out_ctx` are declared before `binsplit_e` (the other nodes call `out_stage` at their
two exit points after the gather / the M2 send: one line each), and the M5 hook after the seeds
is set at size > 1 (`else if (mn_size_ > 1) { bs_after_seeds_hook = pq_bg_start; ... }`). The dm
flow between bs and the output is otherwise unchanged.
