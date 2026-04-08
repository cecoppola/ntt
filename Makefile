# =============================================================================
# Makefile — NTT / MI300A project
#
# Targets:
#   make cpu                  build CT-DIT CPU reference (ntt_cpu)
#   make stockham             build Stockham CPU (ntt_stockham)
#   make mont                 build Montgomery CPU (ntt_mont)
#   make bench                build CPU benchmark sweep (ntt_bench)
#   make polymul              build polynomial multiplication harness (ntt_polymul)
#   make gpu-6900xt           build CT-DIT GPU kernel for 6900XT (gfx1100)
#   make gpu-mi300a           build CT-DIT GPU kernel for MI300A (gfx942)
#   make gpu-stok-6900xt      build Stockham GPU kernel for 6900XT (gfx1100)
#   make gpu-stok-mi300a      build Stockham GPU kernel for MI300A (gfx942)
#   make all                  build all CPU and GPU targets
#   make test-cpu             run CT-DIT CPU selftest
#   make test-stockham        run Stockham CPU selftest
#   make test-mont            run Montgomery CPU selftest
#   make bench-cpu/stockham/mont   run individual CPU benchmarks
#   make bench-sweep          run side-by-side algorithm sweep (all n)
#   make cross-verify         run CPU vs GPU cross-validation (requires 6900XT)
#   make clean                remove all built binaries
#
# Environment variables (override on command line):
#   CC          C compiler for host code  (default: cc)
#   HIPCC       HIP compiler              (default: hipcc)
#   CFLAGS      extra C compiler flags
#   HIPFLAGS    extra hipcc flags
#   N           NTT size for benchmarks   (default: 256)
#   Q           modulus for benchmarks    (default: 3329)
#   OMEGA       primitive root            (default: 17)
#   ITERS       benchmark iteration count (default: 100000)
#
# Cray/HPE target system (MI300A):
#   Load modules before building:
#     module load PrgEng-cray-amd/8.5.0 rocm/7.0.3 craype-accel-amd-gfc942
#   Then: make gpu-mi300a
#   The craype-accel-amd-gfc942 module sets --offload-arch=gfx942 automatically
#   via $PE_OFFLOAD_ARCH; we pass it explicitly below for reproducibility.
# =============================================================================

# ── Compilers ─────────────────────────────────────────────────────────────────
CC    ?= cc
HIPCC ?= hipcc

# ── Base flags ────────────────────────────────────────────────────────────────
# -Wall -Wextra: all warnings required; zero warnings = build clean.
# -O2: optimised but debuggable; bump to -O3 during MI300A tuning phase.
BASE_CFLAGS   := -O2 -Wall -Wextra
BASE_HIPFLAGS := -O2 -Wall -Wextra

# Append any caller-supplied flags
ALL_CFLAGS   := $(BASE_CFLAGS)   $(CFLAGS)
ALL_HIPFLAGS := $(BASE_HIPFLAGS) $(HIPFLAGS)

# ── Offload arch flags ────────────────────────────────────────────────────────
# gfx1100: AMD Radeon RX 6900 XT (RDNA3), wavefront=32, dev/test platform.
# gfx942:  AMD MI300A (CDNA3), wavefront=64, production target.
ARCH_6900XT := --offload-arch=gfx1100
ARCH_MI300A := --offload-arch=gfx942

# ── Benchmark parameters ──────────────────────────────────────────────────────
N     ?= 256
Q     ?= 3329
OMEGA ?= 17
ITERS ?= 100000

# ── Source files ──────────────────────────────────────────────────────────────
CPU_SRC   := ntt_cpu.c
STOK_SRC  := ntt_stockham.c
GPU_SRC   := ntt_gpu.hip
GSTOK_SRC := ntt_gpu_stockham.hip

# ── Output binaries ───────────────────────────────────────────────────────────
CPU_BIN        := ntt_cpu
MONT_BIN       := ntt_mont
STOK_BIN       := ntt_stockham
BENCH_BIN      := ntt_bench
POLYMUL_BIN    := ntt_polymul
GPU_6900XT     := ntt_gpu_6900xt
GPU_MI300A     := ntt_gpu_mi300a
GSTOK_6900XT   := ntt_gpu_stockham_6900xt
GSTOK_MI300A   := ntt_gpu_stockham_mi300a
VERIFY_6900XT  := ntt_cross_verify_6900xt
VERIFY_MI300A  := ntt_cross_verify_mi300a

# ── Default target ────────────────────────────────────────────────────────────
.PHONY: all cpu mont stockham bench polymul \
        gpu-6900xt gpu-mi300a gpu-stok-6900xt gpu-stok-mi300a \
        verify-6900xt verify-mi300a \
        test-cpu test-mont test-stockham test-gpu \
        bench-cpu bench-mont bench-stockham bench-sweep bench-gpu \
        cross-verify cross-verify-mi300a \
        clean

all: cpu mont stockham bench polymul \
     gpu-6900xt gpu-mi300a gpu-stok-6900xt gpu-stok-mi300a \
     verify-6900xt verify-mi300a

# ── Build rules ───────────────────────────────────────────────────────────────

cpu: $(CPU_BIN)

$(CPU_BIN): $(CPU_SRC) ntt.h
	$(CC) $(ALL_CFLAGS) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (CPU reference)"

mont: $(MONT_BIN)

$(MONT_BIN): ntt_mont.c ntt.h
	$(CC) $(ALL_CFLAGS) -o $@ ntt_mont.c
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (Montgomery CPU)"

stockham: $(STOK_BIN)

$(STOK_BIN): $(STOK_SRC) ntt.h
	$(CC) $(ALL_CFLAGS) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (Stockham auto-sort CPU)"

bench: $(BENCH_BIN)

$(BENCH_BIN): ntt_bench.c ntt.h
	$(CC) $(ALL_CFLAGS) -o $@ ntt_bench.c
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (CPU algorithm sweep)"

polymul: $(POLYMUL_BIN)

$(POLYMUL_BIN): ntt_polymul.c ntt.h
	$(CC) $(ALL_CFLAGS) -o $@ ntt_polymul.c
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (polynomial multiplication)"

gpu-6900xt: $(GPU_6900XT)

$(GPU_6900XT): $(GPU_SRC)
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_6900XT) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (gfx1100 / 6900XT)"

gpu-mi300a: $(GPU_MI300A)

$(GPU_MI300A): $(GPU_SRC)
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_MI300A) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (gfx942 / MI300A)"

verify-6900xt: $(VERIFY_6900XT)

$(VERIFY_6900XT): ntt_cross_verify.hip
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_6900XT) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (gfx1100 / 6900XT)"

verify-mi300a: $(VERIFY_MI300A)

$(VERIFY_MI300A): ntt_cross_verify.hip
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_MI300A) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (gfx942 / MI300A)"

gpu-stok-6900xt: $(GSTOK_6900XT)

$(GSTOK_6900XT): $(GSTOK_SRC)
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_6900XT) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (Stockham gfx1100 / 6900XT)"

gpu-stok-mi300a: $(GSTOK_MI300A)

$(GSTOK_MI300A): $(GSTOK_SRC)
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_MI300A) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (Stockham gfx942 / MI300A)"

# ── Test rules ────────────────────────────────────────────────────────────────

test-cpu: $(CPU_BIN)
	@printf '\n  Running CPU selftest (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(CPU_BIN) $(N) $(Q) $(OMEGA) 1

test-mont: $(MONT_BIN)
	@printf '\n  Running Montgomery selftest (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(MONT_BIN) $(N) $(Q) $(OMEGA) 1

test-stockham: $(STOK_BIN)
	@printf '\n  Running Stockham selftest (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(STOK_BIN) $(N) $(Q) $(OMEGA) 1

test-gpu: $(GPU_6900XT)
	@printf '\n  Running GPU selftest (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(GPU_6900XT) $(N) $(Q) $(OMEGA) 1

# Phase 3 exit criterion: CPU vs GPU cross-verification
cross-verify: $(VERIFY_6900XT)
	@printf '\n  Running CPU vs GPU cross-verification (n=$(N) q=$(Q) omega=$(OMEGA))...\n'
	./$(VERIFY_6900XT) $(N) $(Q) $(OMEGA)

cross-verify-mi300a: $(VERIFY_MI300A)
	@printf '\n  Running CPU vs GPU cross-verification on MI300A...\n'
	./$(VERIFY_MI300A) $(N) $(Q) $(OMEGA)

test-gpu-mi300a: $(GPU_MI300A)
	@printf '\n  Running GPU selftest on MI300A (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(GPU_MI300A) $(N) $(Q) $(OMEGA) 1

# ── Benchmark rules ───────────────────────────────────────────────────────────

bench-cpu: $(CPU_BIN)
	@printf '\n  CPU benchmark: n=$(N) q=$(Q) omega=$(OMEGA) iters=$(ITERS)\n'
	./$(CPU_BIN) $(N) $(Q) $(OMEGA) $(ITERS)

bench-mont: $(MONT_BIN)
	@printf '\n  Montgomery benchmark: n=$(N) q=$(Q) omega=$(OMEGA) iters=$(ITERS)\n'
	./$(MONT_BIN) $(N) $(Q) $(OMEGA) $(ITERS)

bench-stockham: $(STOK_BIN)
	@printf '\n  Stockham benchmark: n=$(N) q=$(Q) omega=$(OMEGA) iters=$(ITERS)\n'
	./$(STOK_BIN) $(N) $(Q) $(OMEGA) $(ITERS)

bench-sweep: $(BENCH_BIN)
	@printf '\n  CPU algorithm sweep: all sizes, all algorithms\n'
	./$(BENCH_BIN)

bench-gpu: $(GPU_6900XT)
	@printf '\n  GPU benchmark: n=$(N) q=$(Q) omega=$(OMEGA) iters=$(ITERS)\n'
	./$(GPU_6900XT) $(N) $(Q) $(OMEGA) $(ITERS)

bench-gpu-mi300a: $(GPU_MI300A)
	@printf '\n  GPU (MI300A) benchmark: n=$(N) q=$(Q) omega=$(OMEGA) iters=$(ITERS)\n'
	./$(GPU_MI300A) $(N) $(Q) $(OMEGA) $(ITERS)

# ML-KEM parameter set (n=256, q=3329, omega=17)
bench-mlkem-cpu: $(CPU_BIN)
	./$(CPU_BIN) 256 3329 17 $(ITERS)

bench-mlkem-gpu: $(GPU_6900XT)
	./$(GPU_6900XT) 256 3329 17 $(ITERS)

# ML-DSA parameter set (n=256, q=8380417, omega=1753)
bench-mldsa-cpu: $(CPU_BIN)
	./$(CPU_BIN) 256 8380417 1753 $(ITERS)

bench-mldsa-gpu: $(GPU_6900XT)
	./$(GPU_6900XT) 256 8380417 1753 $(ITERS)

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	rm -f $(CPU_BIN) $(MONT_BIN) $(STOK_BIN) $(BENCH_BIN) $(POLYMUL_BIN)
	rm -f $(GPU_6900XT) $(GPU_MI300A) $(GSTOK_6900XT) $(GSTOK_MI300A)
	rm -f $(VERIFY_6900XT) $(VERIFY_MI300A)
	rm -f bench_*.txt bench_gpu_*.txt
	@printf '  Cleaned.\n'
