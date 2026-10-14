# CKPT — the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run

**Question.** After the 6-node 3.8646 × 10¹¹ run with `ECALC_CKPT_TOP=1` (2026-10-13, VERIFY OK, first 10¹¹ digits
identical), `ECALC_RECHECK=1` on its files printed `bs: checkpoint: short read from <out>.top/n00X_tree_002.rK` for 16 of
the 24 files (all 6 nodes, tree level 2), fell back to the sidecar's P, Q residues, and was killed at its 1800 s timeout.
Was the set written incompletely, read with the wrong size or layout, or is it the file system? Why so slow?

**Change / branch.** Branch `ckpt` (from `main` at 39346d0). Only the top-set writer and reader use `sp_file`
(`ecalc/spill.c`), so nothing else changes:
- `pread_need`: one `pread` asks for at most 64 MiB (`SP_READ_MAX`; it asked for up to 512 MiB); EIO / ETIMEDOUT / EAGAIN /
  ENOMEM: the same read again after 1, 4, 16 s, said on stderr (3 failures in a row fail it); an O_DIRECT read that
  comes back short at a partial block before the bytes needed is read again from that block (it was taken as the end
  of the file).
- `pwrite_full`: the same retry on those errors (no size cap: the writes did not fail).
- `ecalc/binsplit.c`: a failed read names errno, the offset, the bytes got / needed and the file size.
No switch: a correctness fix of the reader (reads that come back whole return the same bytes as before).

**Method.**
1. Code and logs: the writing run's `checkpoint: the top-level P, Q ... 57.25 GB in 1027 s` on every node (the header is
   written only after all four files close without error); the recheck found the level-2 set through `ckpt_find_kind`,
   which checks each file's size against the header — so the files were complete and the sizes right.
2. Queue item `40_ckpt_repro` (2 nodes; a diagnostic build = main + the named failure): a write with `ECALC_CKPT_TOP=1`,
   each file's size against its header, then `ECALC_RECHECK=1` on the same files; 10¹⁰, then 2 × 10¹⁰.
3. Item `41_ckpt_mount` (1 node): the compute node's mount options of the home file system.
4. Items `42_ckpt_gate` (16 MiB cap) and `43_ckpt_gate2` (64 MiB, the merged state): `tools/gate.sh --build --nodes 2`
   and `--nodes 1` (`--line aac7`), then the 2-node write + recheck at 2 × 10¹⁰.

**Results.**

| run | set per node | files vs header | recheck | part-file pass | top-set read | label |
|---|---|---|---|---|---|---|
| 2 nodes, 10¹⁰, main + diagnostics | 4.44 GB, 4 × 1.1 GB | 8/8 equal | RECHECK OK, 0 short reads, 281 s | 164–170 s (12.5 MB/s per node) | 102–108 s (41–44 MB/s) | m |
| 2 nodes, 2 × 10¹⁰, main + diagnostics | 8.89 GB, 4 × 2.2 GB | 8/8 equal | RECHECK OK, 0 short reads, 527 s | 332–336 s (12.5 MB/s) | 180–185 s (48–49 MB/s) | m |
| 2 nodes, 2 × 10¹⁰, 16 MiB cap | 8.89 GB | — | RECHECK OK, 0 short reads, 0 retries, 679 s | 317–370 s | 300–353 s | m |
| 2 nodes, 2 × 10¹⁰, 64 MiB cap | 8.89 GB | — | (pending) | | | m |
| 6 nodes, 3.8646 × 10¹¹ (2026-10-13) | 57.25 GB, 4 × 14.3 GB | equal (found by the size check) | 16 of 24 files short read, killed at 1800 s | | | m |
| 6 nodes, 3.8646 × 10¹¹, the recheck's need | | | ≈ 1 h at the 2-node rates (26.8 GB of parts at 12.5 MB/s ≈ 2140 s + 57.25 GB at 48 MB/s ≈ 1190 s); slower when 6 nodes share the server | | | mod |

Gates on the fix (16 MiB, commit e339948): 2 nodes PASS (10⁹, VERIFY OK, digits identical, P24 in use), 1 node PASS (m).

The cause: the compute nodes mount the home file system NFS **soft** (`timeo=600,retrans=2`, NFSv3, rsize 64 KiB; the
login node mounts it hard). A soft mount fails an RPC that gets no answer within its timeouts with EIO. The reader had
four threads per node, each with one O_DIRECT `pread` of up to 512 MiB in flight: 2 GiB per node, 12 GiB over 6 nodes,
against a server that delivered ≈ 0.3 GB/s in all (the 6-node write: 56 MB/s per node, m) — about 40 s of queue on
average (mod), so the tail of the RPCs passed the 60 s timeout and the reads failed with EIO, which the old message
called a short read. At 2 nodes the queue is a third of that and nothing failed. With 64 MiB requests the 6 nodes have
1.5 GiB in flight, about 5 s of queue (mod). The nodes of the large run had been rebooted, so their kernel logs could
not be checked: the mechanism is inferred (mount options, load), not seen.

The 30 minutes: the recheck reads every part file (26.8 GB per node at 3.8646 × 10¹¹, packed) and then the top set
(57.25 GB per node); the recurrence over the node's terms takes about a second (1.1 s at 2 × 10¹⁰, m; ≈ 7 s at
3.86 × 10¹¹, mod). The sidecar fallback adds nothing: the time is the file system. 1800 s could not have been enough.

**Verdict.** A bug in the reader (it trusted one huge read on a soft-mounted NFS), not in the written set: fixed by
smaller reads plus retries, merged as a correctness fix. The recheck of a large run needs a timeout of ≥ 3 h on aac7
(TESTING.md §3).

**Open issues.**
- Not reproduced at scale: the fix is checked at 2 nodes (no failure before or after); the proof at 6 nodes is the next
  large run's recheck with a ≥ 3 h timeout (`spill: read at offset ...` lines would show retries).
- The part-file pass reads at 12.5 MB/s per node, a quarter of the top set's O_DIRECT rate: the recheck's main cost
  (a candidate for TASKS "Proposed").
- The writer waited 499 s for P before S = P + Q at 3.8646 × 10¹¹ (the 57 GB set at 56 MB/s per node): `ECALC_CKPT_TOP=1`
  costs run time on this file system; `=2` (budgeted) drops the set instead.

**RESUME.** Branch `ckpt`, worktree `../ntt-wt-ckpt`. Done: diagnosis, fix, gates (16 MiB), docs. Pending: item
`43_ckpt_gate2` (64 MiB: gates + 2-node write/recheck), then merge to `main`, push, `~/ntt-acc` pull.
