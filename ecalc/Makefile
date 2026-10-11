# ecalc - the paper's pipeline (PLAN.md 8).   module load rocm && make
ARCH   ?= gfx942
HIPCC  ?= hipcc
HIPFLAGS = -g -O3 --offload-arch=$(ARCH) -fopenmp -I. -I../bench
HIPC     = $(HIPCC) $(HIPFLAGS) -x hip
CFLAGS   = -O2 -Wall -fopenmp -I.
ifeq ($(origin CC),default)
CC       = gcc                       # Phase 16 A: tests/t_comm with gcc unless CC is given (make's built-in `cc` is Cray's wrapper on aac7)
endif
# Phase 11 S: the SHMEM transport (comm_shmem.c) is compiled in when SHMEM=1 (the default where oshcc exists: aac6's
# compute nodes, OpenMPI 4.1.6 OSHMEM; on the target, SHMEM_CFLAGS / SHMEM_LIBS from the vendor's compiler wrapper).
# make SHMEM=0 builds without it (COMM_TRANSPORT=shmem then aborts at start).
# Phase 12 S: SHMEM_HOME=<prefix> builds against Sandia OpenSHMEM (SOS, ~/sos on aac6: OpenSHMEM 1.5, thread-multiple
# contexts, put-with-signal, the external heap patch of results/S12.md) instead of oshcc; the binary is then launched
# by mnrun.sh with srun --mpi=pmi2 (it detects libsma in the binary).  make SHMEM_HOME=~/sos; make clean between the two.
# Phase 16 A (results/A16.md): SHMEM_CRAY=1 builds against Cray OpenSHMEMX (aac7: `module load cray-dsmml cray-openshmemx`,
# OpenSHMEM 1.5, SHMEM_THREAD_MULTIPLE, contexts, put-with-signal) through comm_shmem.c's generic branch (-DCOMM_SHMEM only);
# the prefixes come from the modules (CRAY_OPENSHMEMX_PREFIX, CRAY_DSMML_PREFIX) or CRAY_SMA / CRAY_DSMML on the make line.
# The link is the minimum of `cc --cray-print-opts=libs` (-lsma -lpmi -lpmi2 -ldsmml; PMI from /opt/cray/pe/lib64) with rpaths,
# so the binary runs without the modules; mnrun.sh launches it with plain srun (it detects /opt/cray/pe/sma in the binary).
# GMP_HOME=<prefix> points the tests, tools and ref/gen_e at a GMP built from source (aac7 has libgmp.so.10 but no headers).
SHMEM_HOME ?=
SHMEM_CRAY ?= 0
GMP_HOME   ?=
ifneq ($(GMP_HOME),)
HIPFLAGS += -I$(GMP_HOME)/include
CFLAGS   += -I$(GMP_HOME)/include
GMP_LIBS  = -L$(GMP_HOME)/lib -lgmp -Wl,-rpath,$(GMP_HOME)/lib
else
GMP_LIBS  = -lgmp
endif
ifneq ($(SHMEM_CRAY),0)
SHMEM = 1
CRAY_SMA   ?= $(if $(CRAY_OPENSHMEMX_PREFIX),$(CRAY_OPENSHMEMX_PREFIX),/opt/cray/pe/sma/11.8.0/ofi/sma)
CRAY_DSMML ?= $(if $(CRAY_DSMML_PREFIX),$(CRAY_DSMML_PREFIX),/opt/cray/pe/dsmml/0.3.1/dsmml)
SHMEM_CFLAGS ?= -DCOMM_SHMEM -I$(CRAY_SMA)/include -I$(CRAY_DSMML)/include
SHMEM_LIBS   ?= -L$(CRAY_SMA)/lib64 -L$(CRAY_DSMML)/lib -L/opt/cray/pe/lib64 -lsma -lpmi -lpmi2 -ldsmml -Wl,-rpath,$(CRAY_SMA)/lib64 -Wl,-rpath,$(CRAY_DSMML)/lib -Wl,-rpath,/opt/cray/pe/lib64
else ifneq ($(SHMEM_HOME),)
SHMEM = 1
SHMEM_CFLAGS ?= -DCOMM_SHMEM -DCOMM_SHMEM_SOS -I$(SHMEM_HOME)/include
SHMEM_LIBS   ?= -L$(SHMEM_HOME)/lib -lsma -Wl,-rpath,$(SHMEM_HOME)/lib
else
SHMEM ?= $(shell command -v oshcc > /dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(SHMEM),1)
SHMEM_CFLAGS ?= -DCOMM_SHMEM -DCOMM_SHMEM_OSHMEM $(shell oshcc --showme:compile)
SHMEM_LIBS   ?= $(shell oshcc --showme:link) -lopen-rte -lopen-pal
endif
endif
# Phase 17 (internal code notes): comm_ofi.c, the libfabric data plane of the SHMEM transport (COMM_OFI=1 at run time, off by
# default).  OFI=1 (automatic where $(LIBFABRIC)/include/rdma/fabric.h exists: aac7's /opt/cray/libfabric/2.3.1) compiles it in;
# OFI=0 builds the stubs (COMM_OFI=1 then aborts at start).
LIBFABRIC ?= $(if $(CRAY_LIBFABRIC_PREFIX),$(CRAY_LIBFABRIC_PREFIX),/opt/cray/libfabric/2.3.1)
OFI ?= $(if $(wildcard $(LIBFABRIC)/include/rdma/fabric.h),1,0)
ifeq ($(OFI),1)
OFI_CFLAGS = -DCOMM_OFI -I$(LIBFABRIC)/include
OFI_LIBS   = -L$(LIBFABRIC)/lib64 -lfabric -Wl,-rpath,$(LIBFABRIC)/lib64
SHMEM_LIBS += $(OFI_LIBS)
endif

TESTS = tests/t_modarith tests/t_roots tests/t_ntt tests/t_ntt2 tests/t_mul tests/t_crt tests/t_newton tests/t_bs tests/t_dec tests/t_verify tests/t_dist tests/t_comm tests/t_ntt3 tests/t_dbig tests/t_out tests/t_mn_grid tests/t_alloc tests/t_copy_order tests/t_hred_race tests/t_strategy tests/t_cap tests/t_primes tests/t_spill tests/t_seed tests/t_patch tests/t_p24
OBJS  = ntt.o ntt3.o ntt2.o mem.o crt.o crt2.o comm_local.o comm_sim4.o comm_xgmi.o comm_tcp.o comm_shmem.o comm_ofi.o comm_layered.o comm_util.o ntt_dist.o mn.o mn_out.o bigint.o rns_mul.o rns_dist.o dbig.o newton_db.o newton.o binsplit.o todec.o verify.o mn_plan.o spill.o memsample.o fatal.o segv_trace.o p24_crt.o
REF   = ref/gen_e

all: $(REF) $(OBJS) $(TESTS) ecalc tools

# Phase 15: the packed digit converter / checker (default output since Phase 15) is part of every build
tools:
	$(MAKE) -s -C ../tools

ecalc: ecalc.c $(OBJS) rns_mul.h newton.h binsplit.h todec.h verify.h mem.h ntt.h mn.h mdb.h mn_out.h mn_plan.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(SHMEM_LIBS)

ntt.o: ntt.c ntt.h modarith.h
	$(HIPC) -c -o $@ $<
# S21 (A37-Q4): the NOP-modmul NTT -- ntt.c compiled with -DNTT_NOP_MODMUL (mm_raw := fma(b, 0, a)); only the on-demand bench binary
# tests/t_ntt_nop links it, never ecalc or the tests in TESTS.  Its output is wrong by design; only its timing is read.
ntt_nop.o: ntt.c ntt.h modarith.h
	$(HIPC) -DNTT_NOP_MODMUL -c -o $@ $<
tests/t_ntt_nop: tests/t_ntt.c ntt_nop.o ntt3.o bigint.o fatal.o ntt.h modarith.h tests/harness.h
	$(HIPC) $< -x none ntt_nop.o ntt3.o bigint.o fatal.o -o $@ $(GMP_LIBS)
# S21 (A37-R2): the CPU-only seed-leaf microbench (ecalc's decimal span against a37v1's base-2^64 sub-ranges); on demand, not in TESTS
tests/b_seed64: tests/b_seed64.c bigint.o fatal.o bigint.h
	$(HIPCC) -O3 -fopenmp -I. -x c $< -x none bigint.o fatal.o -o $@ -lm
ntt3.o: ntt3.c ntt.h modarith.h
	$(HIPC) -c -o $@ $<
ntt2.o: ntt2.c ntt2.h modarith2.h
	$(HIPC) -c -o $@ $<
rns_mul.o: rns_mul.c rns_mul.h rns_int.h ntt.h mem.h crt.h bigint.h modarith.h
	$(HIPC) -c -o $@ $<

rns_dist.o: rns_dist.c rns_mul.h rns_int.h ntt_dist.h comm.h mem.h bigint.h dbig.h mdb.h mn_plan.h p24.h p24_mn.h
	$(HIPC) -c -o $@ $<
# Phase 15 Batch 3 P24 (results/P2415.md): the 24-digit CRT of the mn tier (MN_P24)
p24_crt.o: p24_crt.c p24.h rns_int.h modarith.h
	$(HIPC) -c -o $@ $<
tests/t_p24: tests/t_p24.c $(OBJS) p24.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)
# Phase 13d L: the plan printer (MN_PLAN_ONLY; ecalc.c calls it before rns_init)
mn_plan.o: mn_plan.c mn_plan.h mdb.h bigint.h binsplit.h rns_mul.h modarith.h
	$(HIPC) -c -o $@ $<

dbig.o: dbig.c dbig.h bigint.h mem.h p24.h
	$(HIPC) -c -o $@ $<
# Phase 14 S1: O_DIRECT file I/O and the spill primitive (E3, E4); the memory sampler and the live-bytes lines (E12, E1)
spill.o: spill.c spill.h dbig.h mem.h
	$(HIPC) -c -o $@ $<
fatal.o: fatal.c fatal.h
	$(HIPC) -c -o $@ $<
# s18w task 2: ECALC_SEGV_TRACE=1 (default 0) -- see segv_trace.h
segv_trace.o: segv_trace.c segv_trace.h
	$(HIPC) -c -o $@ $<
memsample.o: memsample.c memsample.h dbig.h mem.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
tests/t_spill: tests/t_spill.c $(OBJS) spill.h memsample.h dbig.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)

newton_db.o: newton_db.c newton.h dbig.h rns_mul.h mem.h mdb.h mn.h comm.h mn_plan.h
	$(HIPC) -c -o $@ $<

tests/t_dbig: tests/t_dbig.c $(OBJS) dbig.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)
tests/t_out: tests/t_out.c $(OBJS) mn_out.h verify.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)
tests/t_patch: tests/t_patch.c $(OBJS) mn_out.h verify.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)
tests/t_mn_grid: tests/t_mn_grid.c $(OBJS) dbig.h mdb.h mn.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)
mem.o: mem.c mem.h
	$(HIPC) -c -o $@ $<
comm_local.o: comm_local.c comm.h
	$(HIPC) -c $< -o $@

comm_sim4.o: comm_sim4.c comm.h
	$(HIPC) -c $< -o $@

comm_tcp.o: comm_tcp.c comm.h
	$(HIPC) -c $< -o $@

comm_shmem.o: comm_shmem.c comm.h comm_ofi.h
	$(HIPC) $(SHMEM_CFLAGS) -c $< -o $@

comm_ofi.o: comm_ofi.c comm_ofi.h fatal.h
	$(HIPC) $(OFI_CFLAGS) -c $< -o $@

comm_layered.o: comm_layered.c comm.h
	$(HIPC) -c $< -o $@

mn.o: mn.c mn.h mdb.h comm.h ntt_dist.h ntt.h mem.h dbig.h
	$(HIPC) -c $< -o $@

mn_out.o: mn_out.c mn_out.h verify.h dbig.h comm.h mem.h
	$(HIPC) -c $< -o $@

comm_util.o: comm_util.c comm.h
	$(HIPC) -c $< -o $@

comm_xgmi.o: comm_xgmi.c comm.h
	$(HIPC) -c $< -o $@

tests/t_comm: tests/t_comm.c comm_tcp.c comm_shmem.c comm_ofi.c comm_util.c fatal.c comm.h comm_ofi.h fatal.h
	$(CC) -O2 -DCOMM_HOST_ONLY -DEC_FATAL_NO_HIP $(SHMEM_CFLAGS) $(OFI_CFLAGS) -I. $< comm_tcp.c comm_shmem.c comm_ofi.c comm_util.c fatal.c -lpthread $(SHMEM_LIBS) -o $@

ntt_dist.o: ntt_dist.c ntt_dist.h ntt.h comm.h modarith.h
	$(HIPC) -c $< -o $@
crt2.o: crt2.c crt.h modarith2.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
crt.o: crt.c crt.h modarith.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
todec.o: todec.c todec.h rns_mul.h newton.h mem.h modarith.h
	$(HIPC) -c -o $@ $<
verify.o: verify.c verify.h bigint.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
binsplit.o: binsplit.c binsplit.h bigint.h rns_mul.h mem.h spill.h memsample.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
newton.o: newton.c newton.h bigint.h rns_mul.h mem.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
bigint.o: bigint.c bigint.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<

tests/t_mul: tests/t_mul.c $(OBJS) rns_mul.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)

tests/t_newton: tests/t_newton.c $(OBJS) newton.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)

# Phase 13a S: E0 (the A/B/C strategy boundary, node) and E2 (the plane cap against the grid's cost, arithmetic only)
tests/t_strategy: tests/t_strategy.c $(OBJS) rns_mul.h rns_int.h dbig.h ntt.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)
tests/t_cap: tests/t_cap.c tests/harness.h
	gcc $(CFLAGS) -o $@ $< $(GMP_LIBS) -lm
tests/t_copy_order: tests/t_copy_order.c tests/harness.h
	$(HIPC) $< -o $@ $(GMP_LIBS)
tests/t_hred_race: tests/t_hred_race.c tests/harness.h
	$(HIPC) $< -o $@ $(GMP_LIBS)
tests/t_ntt3: tests/t_ntt3.c $(OBJS) ntt.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)

tests/t_dist: tests/t_dist.c $(OBJS) ntt_dist.h tests/harness.h
	$(HIPC) -fopenmp $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)

# Phase 15 S1: the fast decimal mul_1 and the fused seed span against the serial path
tests/t_seed: tests/t_seed.c $(OBJS) bigint.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) -lm $(SHMEM_LIBS)

tests/t_bs: tests/t_bs.c $(OBJS) binsplit.h tests/harness.h tests/sha256.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) -lm $(SHMEM_LIBS)

tests/t_dec: tests/t_dec.c $(OBJS) todec.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)

tests/t_verify: tests/t_verify.c $(OBJS) verify.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) $(SHMEM_LIBS)

tests/t_primes: tests/t_primes.c $(OBJS) crt.h modarith.h rns_mul.h tests/harness.h
	$(HIPC) -DT_PRIMES_GPU $< -x none $(OBJS) -o $@ $(GMP_LIBS) -lm $(SHMEM_LIBS)

tests/t_crt: tests/t_crt.c crt.o mem.o bigint.o fatal.o crt.h mem.h tests/harness.h
	$(HIPC) $< -x none crt.o mem.o bigint.o fatal.o -o $@ $(GMP_LIBS)

tests/t_ntt: tests/t_ntt.c ntt.o ntt3.o bigint.o fatal.o ntt.h modarith.h tests/harness.h
	$(HIPC) $< -x none ntt.o ntt3.o bigint.o fatal.o -o $@ $(GMP_LIBS)

tests/t_alloc: tests/t_alloc.c ntt.o ntt3.o bigint.o mem.o fatal.o ntt.h mem.h tests/harness.h
	$(HIPC) $< -x none ntt.o ntt3.o bigint.o mem.o fatal.o -o $@ $(GMP_LIBS) -lpthread

tests/t_gather_probe: tests/t_gather_probe.c modarith.h     # S40: k_gather micro-test (results/S40.md); built on demand, not in TESTS; ScratchSize per kernel is printed by the compiler
	$(HIPC) $< -o $@ -Rpass-analysis=kernel-resource-usage

tests/t_edge: tests/t_edge.c     # Phase 16 C: the memory-edge probe (results/C16.md); built on demand, not in TESTS
	$(HIPC) $< -o $@

tests/t_nodechk: tests/t_nodechk.c     # target_kit.sh stage nodechk: per-node meminfo + 1 GB host->device upload per APU; built on demand, not in TESTS
	$(HIPC) $< -o $@

tests/t_ntt2: tests/t_ntt2.c ntt2.o bigint.o fatal.o ntt2.h modarith2.h tests/harness.h
	$(HIPC) $< -x none ntt2.o bigint.o fatal.o -o $@ $(GMP_LIBS)

ref/gen_e: ref/gen_e.c tests/sha256.h
	gcc $(CFLAGS) -o $@ $< $(GMP_LIBS) -lm

# Phase 15 P: the roots above 2^33 (host only, no GPU: runs on the login node)
tests/t_roots: tests/t_roots.c fatal.c fatal.h modarith.h
	gcc $(CFLAGS) -DEC_FATAL_NO_HIP $< fatal.c -o $@ $(GMP_LIBS) -lpthread

tests/t_modarith: tests/t_modarith.c bigint.o fatal.o modarith.h tests/harness.h bigint.h
	$(HIPC) $< -x none bigint.o fatal.o -o $@ $(GMP_LIBS)

clean:
	rm -f $(REF) $(TESTS) $(OBJS) ecalc

.PHONY: all clean tools
tests/t_ofi_alloc: tests/t_ofi_alloc.c comm_ofi.c comm_ofi.h fatal.c fatal.h
	$(CC) -O2 -DCOMM_HOST_ONLY -DEC_FATAL_NO_HIP $(OFI_CFLAGS) -I. $< fatal.c -lpthread -L$(LIBFABRIC)/lib64 -lfabric -o $@
