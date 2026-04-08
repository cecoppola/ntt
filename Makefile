# =============================================================================
# Makefile — NTT / MI300A project
#
# Targets:
#   make cpu          build CPU reference (ntt_cpu) — any host with cc/clang
#   make gpu-6900xt   build GPU kernel for 6900XT (gfx1100)
#   make gpu-mi300a   build GPU kernel for MI300A  (gfx942)
#   make all          build all three
#   make test-cpu     run CPU selftest
#   make test-gpu     run GPU selftest (requires device)
#   make bench-cpu    run CPU benchmark
#   make bench-gpu    run GPU benchmark (requires device)
#   make clean        remove binaries and benchmark output files
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
CPU_SRC := ntt_cpu.c
GPU_SRC := ntt_gpu.hip

# ── Output binaries ───────────────────────────────────────────────────────────
CPU_BIN       := ntt_cpu
GPU_6900XT    := ntt_gpu_6900xt
GPU_MI300A    := ntt_gpu_mi300a

# ── Default target ────────────────────────────────────────────────────────────
.PHONY: all cpu gpu-6900xt gpu-mi300a \
        test-cpu test-gpu bench-cpu bench-gpu \
        clean

all: cpu gpu-6900xt gpu-mi300a

# ── Build rules ───────────────────────────────────────────────────────────────

cpu: $(CPU_BIN)

$(CPU_BIN): $(CPU_SRC)
	$(CC) $(ALL_CFLAGS) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (CPU reference)"

gpu-6900xt: $(GPU_6900XT)

$(GPU_6900XT): $(GPU_SRC)
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_6900XT) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (gfx1100 / 6900XT)"

gpu-mi300a: $(GPU_MI300A)

$(GPU_MI300A): $(GPU_SRC)
	$(HIPCC) $(ALL_HIPFLAGS) $(ARCH_MI300A) -o $@ $<
	@printf '  %-20s %s\n' "BUILD OK:" "$@ (gfx942 / MI300A)"

# ── Test rules ────────────────────────────────────────────────────────────────

test-cpu: $(CPU_BIN)
	@printf '\n  Running CPU selftest (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(CPU_BIN) $(N) $(Q) $(OMEGA) 1

test-gpu: $(GPU_6900XT)
	@printf '\n  Running GPU selftest (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(GPU_6900XT) $(N) $(Q) $(OMEGA) 1

test-gpu-mi300a: $(GPU_MI300A)
	@printf '\n  Running GPU selftest on MI300A (n=$(N) q=$(Q) omega=$(OMEGA) iters=1)...\n'
	./$(GPU_MI300A) $(N) $(Q) $(OMEGA) 1

# ── Benchmark rules ───────────────────────────────────────────────────────────

bench-cpu: $(CPU_BIN)
	@printf '\n  CPU benchmark: n=$(N) q=$(Q) omega=$(OMEGA) iters=$(ITERS)\n'
	./$(CPU_BIN) $(N) $(Q) $(OMEGA) $(ITERS)

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
	rm -f $(CPU_BIN) $(GPU_6900XT) $(GPU_MI300A)
	rm -f bench_*.txt bench_gpu_*.txt
	@printf '  Cleaned.\n'
