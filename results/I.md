# I — the initialisation floor: every allocation form measured, the seeds against the mapping, the 3·2³⁰ planes on the number (Phase 12, PLAN §27 row I)

Branch `i12` (from `main` @ 7aded87; aac6 clone `~/ntt-i`, logs in `~/i12/`). Files: `ecalc/mem.c/.h` (the allocation form),
the pools' creation in `ecalc/rns_mul.c` (the plane switch's default and the verbose line only), the init lines of `ecalc/ecalc.c`
(the C3 block deleted), `binsplit_seeds_begin` in `ecalc/binsplit.c` (the seed order), NEW `ecalc/tests/t_alloc.c` (+ its
Makefile rule). Nothing else touched. Every ecalc run below: `ECALC_VERBOSE=2 RNS_VERBOSE=1`, 4 × 10¹⁰ with the reference
evicted before each run and the digits `cmp`'d against `~/ntt/ecalc/results/e_4e10.out` after it; jobs 20873 (s24-30), 20894 and
20903 (s24-16), 20929, 20934, 20936 (s24-26).

## Summary

| item | result | adopted |
|---|---|---|
| 1. every allocation form (`t_alloc`) | no form maps cheaper than `hipMalloc` in a fresh process: 0.057–0.072 s/GB, the kernel's page work on the calling thread (system time = wall) under one lock; the host forms cost more and route `hipMemcpy` through the SDMA engine (21 GB/s) | `MEM_ALLOC=hipmalloc\|fine\|uncached\|managed\|host\|mmap` in mem.c, default `hipmalloc` |
| 2. the allocator behind the pools, regions, the block pool | one pair (`mem_dev_malloc` / `mem_dev_release`) under every `hipMalloc` of mem.c (`mem_dev_alloc`, `dpool_get`, `dpool_get_exact`); M's tail layout and the accounting untouched | yes (bit-identical in every form tried) |
| 3. the seeds vs the mapping window (`ECALC_SEED_ORDER=overlap\|first\|after`) | overlap 81.5 s, seeds first 88.5, seeds after 88.3: the overlap wins by 7 s because the seeds slow the mapping by only ≈ 1 s while their 14 s of CPU work is hidden | overlap stays the default |
| 4. the 3·2³⁰ planes on the floor | same node, same batch: phases 58.5–59.3 against 63.1–64.8, init +4 s, **wall 80.8 ± 1.3 against 82.0 ± 0.3** | **the size rule is the default** (on below 5 × 10¹⁰ at 2³¹ pools) |
| 5. `ECALC_DM_POOL` / the C3 block | deleted (`ECALC_TAIL=0` remains the fallback; the in-phase-growth report line stays) | yes |

**The floor.** Init at 4 × 10¹⁰ is the bytes the run needs, mapped by the driver at 0.05–0.075 s/GB, with the seed thread's
14 s of CPU work hidden inside it: 252 GB (arenas 132.3 GB in 5.5–7.9 s, plane pools 120 GB in 8–9 s) = 16–19 s with the
planes off, 312 GB = 20–23.5 s with them on. Nothing measured maps cheaper.
What the cost is: **CPU time in the kernel on the allocating thread** — a fresh process's first `hipMalloc` of 28 GB on one
APU takes 2.01 s of wall of which 2.01 s is system time (`t_alloc`'s probe, `getrusage(RUSAGE_THREAD)`), 0.072 s/GB = 14 GB/s,
the rate of one core allocating and clearing pages (on the MI300A the "VRAM" of a hipMalloc is system memory of the APU's
NUMA node, handed out by the amdgpu/TTM allocator in zeroed pages). Four threads allocating on the four APUs at once take
7.9 s of wall for 1.5–2.4 s of system time each: **serialised by one lock** (the same 0.071 s/GB aggregate as one thread
making the four calls, 0.069). The only cheaper mapping seen is a **re-allocation of memory the process freed**: 0.035 s/GB
(the probe run after a 28 GB allocation had been freed: 3.9 s for the same 112 GB, system 1 s per thread) — the runtime or
the driver keeps the freed pages and skips the clearing; that is what A-mem's 0.047 and the 0.035 of an earlier build of this
branch were. So a persistent pool would help only within one process (nothing in the run frees and re-maps at this size:
zero `hipMalloc` inside the phases since M11), and across runs only a daemon holding the memory would — not a design for 576
nodes. `hipMallocAsync` from a mempool with the release threshold at max is the same idea inside one process: its first
allocation costs the same (11.7 s for 200 GB) and a re-allocation after `hipFreeAsync` faulted on this ROCm (an illegal
address in the first kernel over it), so it is not a candidate.

## 1. `tests/t_alloc` — the table (job 20873, s24-30; 50 GB per APU on the four APUs at once, one thread per device as init does)

`./tests/t_alloc 50` (all forms but `async`; `async` on request, `none` = the probe alone). Columns: `alloc` = the wall of
the slowest thread, s/GB = that wall over the bytes of all four APUs (they serialise), `fill` = a first-touch kernel over the
whole buffer, `touch` / `regist` = the mmap forms' CPU first touch on the node and `hipHostRegister`, `bw` = a 16-byte copy
kernel over 25 GB (GB/s, read + write), `ntt` = `ntt_fwd` at 2²⁸ points × 4 (s), `d2d loc` / `pr` = `hipMemcpyAsync` of 8 GiB
between the halves on the same APU / to the next APU's buffer (GB/s), `cpu` = 48 threads of the buffer's node storing 4 GB
into it (GB/s), `free` = the release.

| form | alloc (s) | s/GB | fill | touch | regist | bw GB/s | ntt s | d2d loc | d2d peer | cpu GB/s | free | note |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `hipMalloc` (first in the process) | 14.38 | 0.072 | 0.03 | | | 3029 | 0.049 | 1449 | 91 | 2.6 | 0.53 | 12.56 s / 0.063 in the first run of the day |
| `hipMallocAsync` from a mempool, release threshold max | 11.74 | 0.059 | 0.02 | | | | | | | | 0.78 | the cold allocation; the warm re-allocation after `hipFreeAsync` faulted (illegal access in the fill kernel) |
| `hipMallocManaged` | 22.90 | 0.114 | 0.02 | | | 3065 | 0.049 | **21** | 21 | 31.2 | 0.99 | the copies on the SDMA engine |
| `hipExtMallocWithFlags` uncached | 8.60 | 0.043 | 0.02 | | | 3047 | 0.050 | 1608 | 92 | 2.6 | 0.52 | (after the frees above: warm pages, see the probe) |
| `hipExtMallocWithFlags` fine-grained | 7.03 | 0.035 | 0.02 | | | 3032 | 0.049 | 1591 | 92 | 2.6 | 0.53 | (warm; 0.057–0.066 in a fresh process, job 20873 `b3`) |
| `hipHostMalloc` coherent | 18.47 | 0.092 | 0.02 | | | 2986 | 0.049 | 21 | 21 | 30.7 | 0.73 | |
| `hipHostMalloc` non-coherent | 18.35 | 0.092 | 0.02 | | | 3016 | 0.049 | 21 | 21 | 30.7 | 0.76 | |
| `hipHostMalloc` NumaUser (thread pinned to the node) | 18.14 | 0.091 | 0.02 | | | 3018 | 0.049 | 21 | 21 | 30.6 | 0.73 | |
| mmap + `MADV_HUGEPAGE` + touch on the node + `hipHostRegister` | 16.08 | 0.080 | 0.02 | 2.60 | 13.48 | 3032 | 0.049 | 21 | 21 | 29.9 | 0.86 | the registration is the cost (0.067 s/GB), the touch 0.013 |
| mmap without huge pages | 136.2 | 0.681 | 0.04 | 30.2 | 105.9 | 1818 | 0.126 | 18 | 18 | 33.2 | 13.8 | 4 KiB pages: the GPU runs at 60 % (TLB) |
| `MAP_HUGETLB` | — | | | | | | | | | | | no reserved huge pages on the nodes (`nr_hugepages` 0) |

Read across: **every form runs the kernels at HBM speed** (bw 3.0 TB/s, the 2²⁸ transform 49 ms — the same to the ms) except
4 KiB-paged host memory; the host-side forms (`hipHostMalloc`, mmap + register, managed) let the CPU store 12 × faster
(30 GB/s against 2.6 — the seeds' stores would gain) **but `hipMemcpy` between two such buffers goes to the SDMA engine at
21 GB/s** (the blit kernels do 1.4–1.6 TB/s on device memory), which the checkpoints, the tree's slab copies and every
`mem_dev_copy` would pay — and they map no cheaper (0.08–0.11 s/GB). The device forms (`hipMalloc`, fine, uncached) are
the same memory at the same cost: the 0.035–0.043 of the two `hipExtMallocWithFlags` rows is the warm re-allocation of the
200 GB the earlier rows had freed, not the form — a fresh process pays 0.057–0.066 for them too (`b3.out`: hipmalloc / fine /
hipmalloc / fine / uncached / hipmalloc at 28 GB per APU: 0.065, 0.066, 0.064, 0.057, 0.063, 0.057 — the run-to-run noise,
no ordering by form). That is why `MEM_ALLOC`'s default went from `fine` (the first commit, on the 0.035 reading) back to
`hipmalloc`: the 4 × 10¹⁰ runs with `fine` (init 18.5 / 19.2 s, wall 84.9 / 85.1) were no faster than `hipmalloc` (16.0 /
17.7 / 18.1 / 17.3, wall 82.3 / 81.5 / 82.3 / 82.1) on the same node.

**The probe (fresh process, `t_alloc 28 none`, job 20903, s24-16):** four threads on the four APUs, 28 GB each: wall 7.6–7.9 s
per thread, user 0, system 1.5–2.4 s — 7.9 s of wall for 112 GB (0.071 s/GB) with the four threads mostly *waiting*; one
thread making the four calls: 7.77 s (0.069); one thread, one APU, 28 GB: wall 2.01 s = system 2.01 s (0.072 s/GB). The
mapping is CPU work in the kernel on the caller's thread, serialised across callers by a lock: neither more threads nor
another API changes it. (Four processes would be the test of a per-process lock; a fork of a HIP process cannot allocate,
and one node-process per node is the design, so it was not pursued.)

**The seed-like team (`t_alloc 50`, the second part):** `hipMalloc` of 200 GB while a team of compute-bound threads runs —
none: 11.25 s (0.056 s/GB); all 192 cpus computing only: 15.7 s (0.079); all cpus computing *and storing into device memory*
(the seeds' pattern, here at the team's full store rate of 200 GB/s): 44 s (0.221); 4 or 8 cpus left free: 56 / 38 s; half the
cpus: 42 s. So the mapping is slowed 1.4 × by CPU contention and 4 × by CPU stores into device memory *at the team's full rate*
— but the real seeds store only ≈ 6 GB/s (their arithmetic is the work), and in the run the plane pools map at 0.067–0.075
s/GB under the seeds against 0.064 alone (item 3 below): the contention P saw (0.08–0.12 s/GB) is mostly the fresh-process
cost itself plus the pool lock, not the seeds.

## 2. `MEM_ALLOC` — the allocator (mem.c)

`mem_dev_malloc(dev, bytes)` / `mem_dev_release(dev, p)` are the pair every large device allocation goes through: `mem_dev_alloc`
(the regions / arenas, the block pool's chunks through `db_pregrow`'s own hipMalloc are dbig.c's — untouched, zero of them
since M11), `dpool_get` and `dpool_get_exact` (the plane pools), `dpool_free`, `mem_dev_free`, `mem_dev_free_raw`. The form
is `MEM_ALLOC`: `hipmalloc` (default), `fine`, `uncached`, `managed`, `host` (NumaUser, the thread pinned to the APU's node),
`mmap` (anonymous, `MADV_HUGEPAGE`, first touch by the node's threads, `hipHostRegister`; a table keeps the lengths for
`munmap`). The verbose pool line prints the form. `mem_oom` names it. Nothing else changed: M's tail layout, the donation,
the accounting (`mem_report`) see the same pointers.

Tried at 4 × 10¹⁰ (job 20873, s24-30, one batch): `fine` 84.9 / 85.1 s (init 18.5 / 19.2), `hipmalloc` 82.3 (init 16.0);
identical digits, VERIFY OK. The host forms were not run through ecalc: the 21 GB/s copies rule them out before the wall does.

## 3. The seeds against the mapping window (`ECALC_SEED_ORDER`, job 20894, s24-16, one batch)

`binsplit_seeds_begin` (the `rns_after_staging_hook`, inside `rns_init` after the staging): `overlap` (default) starts the seed
thread, maps the arenas, posts them to the thread, returns — the plane pools map while the seeds stream; `first` maps the
arenas, then joins the seed thread before returning (nothing maps while the seeds run, the plane pools map alone after);
`after` maps the arenas and returns without starting the seeds — they run synchronously in `binsplit_e` with everything mapped.

| 4 × 10¹⁰ | init | of it: arenas / seed wait / plane pools | seeds (where) | bs | dm | phases | **wall** |
|---|---:|---|---|---:|---:|---:|---:|
| (c) overlap (default) | 17.7 | 6.9 / — / 8.8 (0.073 s/GB under the seeds) | 16.4 s in init (spans 8.5, waited 5.5 for the regions, 1.7 issuing the two DMAs) | 35.2 | 28.5 | 63.8 | **81.5** |
| (b) seeds first, then the pools | 24.2 | 6.8 / 7.6 / 8.1 (0.067 alone) | 14.4 s in init (joined before the pools) | 35.6 | 28.7 | 64.4 | 88.5 |
| (a) mapping first, then the seeds | 16.2 | 6.8 / — / 7.7 (0.064 alone) | 9.1 s in bs (spans 8.5, all direct) | 43.9 | 28.2 | 72.1 | 88.3 |

The wall decides for the overlap: the plane pools map only 0.7–1.1 s slower under the seeds (8.8 against 7.7–8.1 s for 120
GB), while moving the seeds out of the window puts 7–9 s of them on the wall. (The seeds alone are 9 s of spans with the
regions mapped; in the overlap they wait 5.5 s for the regions and issue their two buffered DMAs 1.7 s late behind the
runtime's lock — 16.4 s of thread time for 8.5 s of work, all inside init's 17.7.) `BS_SEED_THREADS=96` (half the cpus, to
leave the mapping thread alone) made the spans 13.9 s and the wall 83.7; 176: 82.0 — no gain either way. Digits identical
in all three orders.

## 4. The 3·2³⁰ planes on the floor (job 20903, s24-16, one batch, the reference evicted before each)

| 4 × 10¹⁰ | init (arenas / pools) | bs (batch / top levels) | recip | dm | phases | **wall** | digits |
|---|---|---|---:|---:|---:|---:|---|
| `RNS_PLANES_3Q30=1` (planes 24 + 18 GiB per APU, 180 GB) | 22.5 (7.3 / 13.4 = 0.074 s/GB) | 32.7 (22.1 / 10.5) | 12.5 | 26.4 | **59.2** | **81.7** | identical, VERIFY OK |
| planes off (16 + 12 GiB, 120 GB), run 1 | 18.1 (7.4 / 9.1) | 35.2 (22.5 / 12.6) | 13.5 | 28.9 | 64.2 | 82.3 | identical, VERIFY OK |
| planes off, run 2 | 17.3 (6.3 / 9.3) | 35.8 (22.8 / 12.8) | 13.6 | 28.9 | 64.8 | 82.1 | identical, VERIFY OK |

The 60 GB more map at the same 0.074 s/GB (+4.4–5.2 s of init) and the phases gain 5.0–5.6 s — P's 3.6 s in the top levels
and the reciprocal, plus 2.5 s in the division that M11's tail layout uncovered (the division's products on 3·2³⁰ planes
with nothing mapped inside the phase). Net −0.4…−0.6 s of wall: by P's rule ("on below 5 × 10¹⁰ if it wins") **the size rule
is now the default** (`rns_planes_3q30_default`: on at 2³¹ pools below 5 × 10¹⁰ digits; `RNS_PLANES_3Q30=0/1` overrides;
7–8 × 10¹⁰ do not fit with them and stay off). The gain is within one run's noise on the wall but not on the phases (5 s,
every tier), and it is the fastest phase time of this code (59.2 s; P's 62.4 on the old layout). The final-default runs
are in §6.

## 5. `ECALC_DM_POOL` deleted

The C3 block in `ecalc.c` (the block pool pre-grown to the reciprocal's scratch, on from 5 × 10¹⁰) is gone: with the tail
layout t1's quarter lands in the arena's reserved tail and the pool never grows (zero `hipMalloc` in every phase at 4, 8 ×
10¹⁰ here, as in M11). `ECALC_TAIL=0` is the fallback (the Phase 10 layout, the pool growing inside the phase), and the
"recip: the block pool grew by … inside the phase" line stays as the check. `ECALC_DM_POOL_K` (the arena sized at init) is
untouched.

## 6. The gate

| test | command (from `ecalc/`) | result |
|---|---|---|
| 10⁹ decimal, binary | `./ecalc 1000000000 /tmp/x` ; `LIMB_BASE=2` | identical to `ref/e_1000000000.txt` in every build: 14.3 / 24.7 s with the planes off, 18.2 / 23.8 on the final defaults, and identical with `MEM_ALLOC=fine` |
| 4 × 10¹⁰, `MEM_ALLOC=fine` × 2 | job 20873 | identical, VERIFY OK, 84.9 / 85.1 s (init 18.5 / 19.2) |
| 4 × 10¹⁰, `MEM_ALLOC=hipmalloc`, planes off × 5 | jobs 20873, 20894, 20903 | identical, VERIFY OK: 82.3 / 81.5 / 82.0 / 82.3 / 82.1 s (init 16.0 / 17.7 / 18.2 / 18.1 / 17.3) — **82.0 ± 0.3 s, init 17.5 ± 0.9** |
| 4 × 10¹⁰, the seed orders, `BS_SEED_THREADS` 96 / 176 | job 20894 | identical (§3) |
| 4 × 10¹⁰, planes on (the final default) × 6 | jobs 20903, 20929, 20934 | identical, VERIFY OK: 81.7 / 80.4 / 78.8 / 82.2 / 81.7 / 79.8 s (§6b) |
| 8 × 10¹⁰ × 2 | `./ecalc 80000000000 /tmp/x` (planes off by the size rule) | **VERIFY OK, 192.0 and 189.3 s** (init 20.9 / 23.1: arenas 248.2 GB in 9.7–10.8 s, plane pools 18.5–21.2; bs 86.3 / 83.6, dm 84.7 / 82.5), **device 369.1 GB throughout, zero hipMalloc on every APU, host 13.0: node peak ≈ 382 GB** (M11: 190.9–195.5 s, the same 369.1 / 382) |
| `t_ntt 24` | `./tests/t_ntt 24` | VERIFY OK (565 checks); 2²⁴ fwd 0.6–0.7 ms (1.22–1.29 TB/s), batched log L 14 / 17: 1026–1219 / 1068–1105 GB/s (P's log: 1006–1058 / 914–1022) |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J ./mnrun.sh <p> env POOL_LOG=27 ./ecalc 100000000 ~/i12/mn/x` | every node VERIFY OK, the concatenated parts identical to `ref/e_100000000.txt` (jobs 20903, 20936) |
| `t_alloc` | `./tests/t_alloc 50`, `28 none`, `28 <form>` | §1 |

### 6b. The final defaults (planes by the size rule, `MEM_ALLOC=hipmalloc`, seeds overlapped; jobs 20903 s24-16, 20929 s24-26)

| run | node | init | bs (batch / top levels) | recip | dm | phases | **wall** | digits |
|---|---|---:|---|---:|---:|---:|---:|---|
| 1 | s24-16 | 22.5 | 32.7 (22.1 / 10.5) | 12.5 | 26.4 | 59.2 | 81.73 | identical, VERIFY OK |
| 2 | s24-26 | 21.9 | 32.5 (21.9 / 10.5) | 12.3 | 26.0 | 58.5 | 80.44 | identical, VERIFY OK |
| 3 | s24-26 | 20.1 | 32.6 (22.0 / 10.4) | 12.2 | 26.0 | 58.7 | 78.81 | identical, VERIFY OK |
| 4 | s24-26 | 23.5 | 32.6 (21.9 / 10.5) | 12.3 | 26.1 | 58.7 | 82.19 | VERIFY OK (the job's time limit cut the `cmp`) |
| 5 | s24-26 | 23.1 | 32.6 (22.0 / 10.5) | 12.2 | 26.0 | 58.6 | 81.71 | identical, VERIFY OK |
| 6 | s24-26 | 20.5 | 33.0 (22.3 / 10.5) | 12.4 | 26.3 | 59.3 | 79.77 | identical, VERIFY OK |
| planes off on the same node (`RNS_PLANES_3Q30=0`) | s24-26 | 18.9 | 34.9 | 13.2 | 28.1 | 63.1 | 81.99 | identical, VERIFY OK |

**Wall 80.8 ± 1.3 s, phases 58.8 ± 0.3, init 22.1 ± 1.3** (six runs) against the Phase 11 close (81.5 ± 1.4, phases 66.0 ± 0.4, init
15.5 ± 1.1) and against this branch with the planes off (82.0 ± 0.3, phases 63.8–64.8, init 17.3–18.9): **the phases are
5–7 s faster and the wall 1 s**, all of the phase gain being M11's layout (−2 s: no in-phase mapping) plus the 3·2^k planes
(−5 s), and all of the init cost being the 60 GB they map.

## Init before / after (4 × 10¹⁰)

| | Phase 11 close (P's gate, s24-26) | this branch, planes off (s24-16/30) | this branch, planes on (default) |
|---|---:|---:|---:|
| staging + contexts | 1.0–1.1 | 1.0–1.2 | 0.9 |
| arenas (132.3 GB) | 4.8–5.3 (95.7 GB) | 5.8–7.5 | 7.3 |
| plane pools | 12.4–15.7 (120 GB, the arenas' time included in P's line) | 8–9 (120 GB) | 13.4 (180 GB) |
| peer access | 0.3 | 0.3–0.6 | 0.7 |
| init | 15.5 ± 1.1 | 16.0–18.2 | 22.5 |
| phases / wall | 66.0 / 81.5 | 63.8–64.8 / 82.0 ± 0.3 | 59.2 / 81.7 |

(The arenas are 132.3 GB since M11 against P's 95.7: the tail layout's 36.6 GB of dm extra, which bought the zero in-phase
growth; the plane pools' 8–9 s is the same 120 GB at the same rate as P's once the arenas are taken out of P's "pools"
figure.) The floor is 0.06–0.075 s/GB × the bytes mapped at init — 252 GB → 15–16 s, 312 GB with the planes → 21 s — and
the seeds fit inside it. The bytes are M's (sized to the phases' need with zero growth); the rate is the driver's.

## Open issues

* The mapping rate is the kernel's page clearing on one core under one lock; a driver that clears pages on the SDMA engines,
  or hands out pre-cleared pages from a pool, would be the fix — not ours. `amdgpu`'s TTM page pool keeps freed pages (the
  0.035 s/GB of a re-allocation) but a fresh process starts empty.
* `hipMallocAsync` / `hipMemPool`: the warm re-allocation faulted on this ROCm; not chased.
* The seeds' two buffered chunks (the first 4 GB, computed before the regions exist) wait 1.4–1.8 s behind the runtime's lock
  to issue their DMAs; harmless inside init's wall, gone if the arenas were mapped before the seed thread starts (then the
  first chunks wait 7 s for the regions instead — worse).
* With the planes on, init is 22.5 s of which the seed thread is 21 s: any further cut of the mapping would expose the
  seeds (14 s of CPU work, 8.5 of spans) — the next item after the mapping is the seeds' own rate, not measured here.
* `t_alloc`'s host forms show the CPU storing into host-registered memory at 30 GB/s against 2.6 into device memory: a
  region for the seeds in host memory would make their stores free, but the region is then read by the batch tier at HBM
  speed only from its own APU (same as now) and copied at 21 GB/s by any `hipMemcpy` — not tried.
