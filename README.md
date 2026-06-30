[1;37m╔════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║                                       N T T   /   M I 3 0 0 A   —   R E A D M E                                        ║
╚════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝[0m

[1;33m  View with:  cat README.md   or   less -R README.md[0m

  High-performance modular Number-Theoretic Transform for AMD MI300A.
  Pure C / HIP; develops on a 6900 XT (gfx1030), cross-compiles for
  MI300A (gfx942). The deliverable (lib/) is a 4-prime CRT-NTT engine for
  big-integer multiplication on a K4 node of MI300As, demonstrated by
  app/compute_e (Euler's number to N digits). ref/ is a single-prime
  reference (ML-KEM / ML-DSA / Goldilocks NTTs).

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  1. AT A GLANCE[0m

  ┌─────────────────┬────────────────────────────────────────────────────────┐
  │ [1;36mField[0m           │ [1;36mValue[0m                                                  │
  ├─────────────────┼────────────────────────────────────────────────────────┤
  │ Goal            │ Highest-throughput modular NTT for MI300A APU          │
  │ Languages       │ C (host), HIP (device); no C++ in source               │
  │ Toolchain       │ ROCm 7.0.3 / hipcc 7.0.51831 / AMD clang 20            │
  │ Dev hardware    │ AMD Radeon RX 6900 XT (gfx1030, RDNA2)                 │
  │ Target hardware │ AMD Instinct MI300A (gfx942, CDNA3, wave 64, HBM3 UMA) │
  │ Status          │ Delivery-ready: gfx942 xcompile clean; log_n=24 PASS   │
  └─────────────────┴────────────────────────────────────────────────────────┘

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  2. PROJECT DOCUMENT MAP[0m

  ★ Fresh agent? Read AGENT_BRIEF.md first — one dense file with the whole
    picture (goal, state, mental model, objectives, landmines, key data).

  ┌────────────────────────────────────────────────┬──────────────────────────────────────────────────────────────┐
  │ [1;36mDocument[0m                                       │ [1;36mRead when[0m                                                    │
  ├────────────────────────────────────────────────┼──────────────────────────────────────────────────────────────┤
  │ README.md (this file)                          │ first contact with the project                               │
  │ ARCHITECTURE.md                                │ design + APIs + moduli + CRT engine + MI300A optim checklist │
  │ PERFORMANCE.md                                 │ current measured baseline per hardware target                │
  │ ROADMAP.md                                     │ phase status, open MI300A tasks, future work                 │
  │ CLAUDE.md                                      │ agent guidance (only relevant for agent-driven sessions)     │
  │ ref/README.md, lib/README.md, app/README.md │ per-layer overviews                                          │
  │ mi300a_environment_0509.txt                    │ definitive MI300A environment reference (2026-05-09)         │
  │ ~/MI300A_TARGET_ENVIRONMENT.md                 │ cross-project MI300A reference                               │
  │ ~/HIP_6900XT_KNOWLEDGE.md                      │ cross-project 6900 XT knowledge base                         │
  └────────────────────────────────────────────────┴──────────────────────────────────────────────────────────────┘

[1;35m  3. QUICK BUILD[0m

  [1;36mLocal dev box (6900 XT / gfx1030):[0m
    make all                          # build the ref CPU binaries
    make check                        # GPU-free host gate (17 suites)
    make all-mi300a                   # gfx942 cross-compile proof

  [1;36mMI300A cross-compile (gfx942) from the dev box:[0m
    make all-mi300a                   # compile-only proof of every gfx942 TU

  [1;36mOn the MI300A (Cray PE):[0m
    module load PrgEnv-amd/8.6.0 rocm/7.0.3 craype-accel-amd-gfx942 \
                cray-mpich/9.0.1 cray-shmem/12.0.0 gcc/14.3.0
    cd lib && make all                # crt_ntt (engine) + test_ntt
    cd ../app/compute_e && make       # the compute_e demonstrator
    srun -p mi -N 1 -n 1 ./lib/test_ntt                            # 8-test suite
    srun -p mi -N 1 -n 4 --ntasks-per-node=4 ./lib/crt_ntt 20 100  # 4-APU bench

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  4. SOURCE LAYOUT[0m

  ref/      single-prime CPU reference NTT + GPU parity kernels (not shipped)
  lib/      THE deliverable: 4-prime CRT-NTT engine + arith layer (gfx942 K4)
  app/      applications/demonstrators (compute_e: e to N digits)
  scripts/  host test/CI gates + MI300A setup/acceptance
  perf/      local-only bench/results/probes (gitignored)
  archive/   retired/dev-only material (gitignored): docs, lib_dead, devbox scripts
  bin/       build artifacts (gitignored)

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  5. SAFETY POSTURE (memory-backed)[0m

  [1;31m●[0m NEVER SIGKILL rocprofv3 — leaves stuck queues, delayed reset.
  [1;31m●[0m NEVER loop rocprofv3 across binaries unsupervised.
  [1;33m●[0m On the 6900 XT (display + compute on one card), a GPU reset
    blackscreens the desktop.
  [1;33m●[0m Build with -Wall -Wextra; fix every warning before marking a unit done.
  [1;32m●[0m t_atomic and t_fabric have GFX1030_LOCAL-gated fixes; do not unwind them.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m


[1;35m  6. BUILD ENVIRONMENT[0m

  [1;36mCray PE modules (MI300A side — confirmed on baryon-cn0063, 2026-05-09):[0m

  PrgEnv-amd/8.6.0  amd/7.0.3  rocm/7.0.3  craype/2.7.35
  craype-x86-genoa  craype-accel-amd-gfx942  craype-network-ofi
  cray-mpich/9.0.1  cray-shmem/12.0.0  cray-pmi/6.1.16
  cray-libsci/25.09.0  cray-libsci_acc/25.09.2  cray-dsmml/0.4.0
  libfabric/1.22.0  perftools-base/25.09.0  gcc/14.3.0
  llvm/18.1.1 (system — DO NOT use for HIP)

  ~/.bashrc on MI300A: [1;36mmodule load rocm/7.0.3[0m (Cray PE loads the rest via PrgEnv-amd)

  [1;36mCaveats:[0m
    [1;31m●[0m cray-mpich/9.0.1 built against ROCm 6.0 — avoid MPI in GPU paths
    [1;31m●[0m NEVER wildcard-link ROCm (-l*) — perftools-base intercepts via rocprof-sys
    [1;31m●[0m Direct ./binary execution fails on MI300A — cgroup forces all jobs through srun

  [1;36msrun patterns:[0m
    Single APU:   srun -p mi -N 1 -n 1 ./bin/binary
    All 4 APUs:   srun -p mi -N 1 -n 4 --ntasks-per-node=4 ./bin/binary
    Two nodes:    srun -p mi -N 2 -n 8 --ntasks-per-node=4 ./bin/binary

  [1;36mMakefile arch flags:[0m
    ARCH_6900XT := --offload-arch=gfx1030 -DGFX1030_LOCAL=1
    ARCH_MI300A := --offload-arch=gfx942  -DMFMA_TARGET=1

  [1;36mKey targets:[0m  make all · make cpu · make all-mi300a · make gpu-mi300a
    make gpu-stok-mi300a · make gpu-polymul-mi300a · make verify-mi300a · make clean
    [1;36mmake check[0m       — GPU-free reliability gate (17 host suites + ASAN/
                      UBSan + fault-injection proof + 90% coverage floor)
    [1;36mmake coverage-all[0m — gcov across ref+lib+app (80% per-TU floor)
    [1;36mmake cpu-all[0m     — build + selftest + bench every ref CPU binary
                      (dev-box GPU runs use scripts/gpu_run.sh, local-only)

  [1;36mMI300A measured baselines:[0m  HBM 4.0 TB/s · LDS 5.0 TB/s · FP64 DGEMM 87.6 TF/s
    SDMA 3.98 TB/s · P2P all-to-all 4.62 TB/s · Atomic 912 GOPS/s

  [1;36mTroubleshooting:[0m
    perftools-base interference: use explicit -l flags, never -l*
    libamdhip64.so.6 not found: MPI GTL shim expects ROCm 6; build without MPI
    HIP fat-binary non-deterministic: use disassembly hash, not binary hash

[1;37m════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  7. ALGORITHM CHOICES[0m

  Implemented stack — all segments resolved. Per-segment rationale and
  measured assumptions: see ARCHITECTURE.md §10 (Design Rationale).

  ┌─────┬────────────────────────────────────────────────────────────────────────┐
  │ [1;36mSeg[0m │ [1;36mChoice[0m                                                                 │
  ├─────┼────────────────────────────────────────────────────────────────────────┤
  │ 1   │ Cooley-Tukey DIT (CPU ref) + Stockham LDS-fused (GPU primary)          │
  │ 2   │ Shoup precomputed-quotient (GPU) · Montgomery REDC (lib) · lazy (CPU) │
  │ 3   │ Bit-reversal eliminated on Stockham path; separate kernel on CT-DIT    │
  │ 4   │ Twiddles precomputed in HBM; LDS-cached for n ≤ 4096                   │
  │ 5   │ Fused multi-stage LDS kernel + MI300A unified HBM pointer              │
  │ 6   │ Grid-stride batch · LDS-batched batch · fused polymul                  │
  └─────┴────────────────────────────────────────────────────────────────────────┘
[1;37m════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m
