# ecalc - the paper's pipeline (PLAN.md 8).   module load rocm && make
ARCH   ?= gfx942
HIPCC  ?= hipcc
HIPFLAGS = -g -O3 --offload-arch=$(ARCH) -fopenmp -I. -I../bench
HIPC     = $(HIPCC) $(HIPFLAGS) -x hip
CFLAGS   = -O2 -Wall -fopenmp -I.
# Phase 11 S: the SHMEM transport (comm_shmem.c) is compiled in when SHMEM=1 (the default where oshcc exists: aac6's
# compute nodes, OpenMPI 4.1.6 OSHMEM; on the target, SHMEM_CFLAGS / SHMEM_LIBS from the vendor's compiler wrapper).
# make SHMEM=0 builds without it (COMM_TRANSPORT=shmem then aborts at start).
# Phase 12 S: SHMEM_HOME=<prefix> builds against Sandia OpenSHMEM (SOS, ~/sos on aac6: OpenSHMEM 1.5, thread-multiple
# contexts, put-with-signal, the external heap patch of results/S12.md) instead of oshcc; the binary is then launched
# by mnrun.sh with srun --mpi=pmi2 (it detects libsma in the binary).  make SHMEM_HOME=~/sos; make clean between the two.
SHMEM_HOME ?=
ifneq ($(SHMEM_HOME),)
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

TESTS = tests/t_modarith tests/t_ntt tests/t_ntt2 tests/t_mul tests/t_crt tests/t_newton tests/t_bs tests/t_dec tests/t_verify tests/t_dist tests/t_comm tests/t_ntt3 tests/t_dbig tests/t_out tests/t_mn_grid tests/t_alloc tests/t_copy_order tests/t_hred_race tests/t_strategy tests/t_cap tests/t_primes tests/t_spill
OBJS  = ntt.o ntt3.o ntt2.o mem.o crt.o crt2.o comm_local.o comm_sim4.o comm_xgmi.o comm_tcp.o comm_shmem.o comm_layered.o comm_util.o ntt_dist.o mn.o mn_out.o bigint.o rns_mul.o rns_dist.o dbig.o newton_db.o newton.o binsplit.o todec.o verify.o mn_plan.o spill.o memsample.o fatal.o
REF   = ref/gen_e

all: $(REF) $(OBJS) $(TESTS) ecalc

ecalc: ecalc.c $(OBJS) rns_mul.h newton.h binsplit.h todec.h verify.h mem.h ntt.h mn.h mdb.h mn_out.h mn_plan.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(SHMEM_LIBS)

ntt.o: ntt.c ntt.h modarith.h
	$(HIPC) -c -o $@ $<
ntt3.o: ntt3.c ntt.h modarith.h
	$(HIPC) -c -o $@ $<
ntt2.o: ntt2.c ntt2.h modarith2.h
	$(HIPC) -c -o $@ $<
rns_mul.o: rns_mul.c rns_mul.h rns_int.h ntt.h mem.h crt.h bigint.h modarith.h
	$(HIPC) -c -o $@ $<

rns_dist.o: rns_dist.c rns_mul.h rns_int.h ntt_dist.h comm.h mem.h bigint.h dbig.h mdb.h mn_plan.h
	$(HIPC) -c -o $@ $<
# Phase 13d L: the plan printer (MN_PLAN_ONLY; ecalc.c calls it before rns_init)
mn_plan.o: mn_plan.c mn_plan.h mdb.h bigint.h binsplit.h rns_mul.h modarith.h
	$(HIPC) -c -o $@ $<

dbig.o: dbig.c dbig.h bigint.h mem.h
	$(HIPC) -c -o $@ $<
# Phase 14 S1: O_DIRECT file I/O and the spill primitive (E3, E4); the memory sampler and the live-bytes lines (E12, E1)
spill.o: spill.c spill.h dbig.h mem.h
	$(HIPC) -c -o $@ $<
fatal.o: fatal.c fatal.h
	$(HIPC) -c -o $@ $<
memsample.o: memsample.c memsample.h dbig.h mem.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
tests/t_spill: tests/t_spill.c $(OBJS) spill.h memsample.h dbig.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)

newton_db.o: newton_db.c newton.h dbig.h rns_mul.h mem.h mdb.h mn.h comm.h mn_plan.h
	$(HIPC) -c -o $@ $<

tests/t_dbig: tests/t_dbig.c $(OBJS) dbig.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)
tests/t_out: tests/t_out.c $(OBJS) mn_out.h verify.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)
tests/t_mn_grid: tests/t_mn_grid.c $(OBJS) dbig.h mdb.h mn.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)
mem.o: mem.c mem.h
	$(HIPC) -c -o $@ $<
comm_local.o: comm_local.c comm.h
	$(HIPC) -c $< -o $@

comm_sim4.o: comm_sim4.c comm.h
	$(HIPC) -c $< -o $@

comm_tcp.o: comm_tcp.c comm.h
	$(HIPC) -c $< -o $@

comm_shmem.o: comm_shmem.c comm.h
	$(HIPC) $(SHMEM_CFLAGS) -c $< -o $@

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

tests/t_comm: tests/t_comm.c comm_tcp.c comm_shmem.c comm_util.c fatal.c comm.h fatal.h
	$(CC) -O2 -DCOMM_HOST_ONLY -DEC_FATAL_NO_HIP $(SHMEM_CFLAGS) -I. $< comm_tcp.c comm_shmem.c comm_util.c fatal.c -lpthread $(SHMEM_LIBS) -o $@

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
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)

tests/t_newton: tests/t_newton.c $(OBJS) newton.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)

# Phase 13a S: E0 (the A/B/C strategy boundary, node) and E2 (the plane cap against the grid's cost, arithmetic only)
tests/t_strategy: tests/t_strategy.c $(OBJS) rns_mul.h rns_int.h dbig.h ntt.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)
tests/t_cap: tests/t_cap.c tests/harness.h
	gcc $(CFLAGS) -o $@ $< -lgmp -lm
tests/t_copy_order: tests/t_copy_order.c tests/harness.h
	$(HIPC) $< -o $@ -lgmp
tests/t_hred_race: tests/t_hred_race.c tests/harness.h
	$(HIPC) $< -o $@ -lgmp
tests/t_ntt3: tests/t_ntt3.c $(OBJS) ntt.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)

tests/t_dist: tests/t_dist.c $(OBJS) ntt_dist.h tests/harness.h
	$(HIPC) -fopenmp $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)

tests/t_bs: tests/t_bs.c $(OBJS) binsplit.h tests/harness.h tests/sha256.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp -lm $(SHMEM_LIBS)

tests/t_dec: tests/t_dec.c $(OBJS) todec.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)

tests/t_verify: tests/t_verify.c $(OBJS) verify.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp $(SHMEM_LIBS)

tests/t_primes: tests/t_primes.c $(OBJS) crt.h modarith.h rns_mul.h tests/harness.h
	$(HIPC) -DT_PRIMES_GPU $< -x none $(OBJS) -o $@ -lgmp -lm $(SHMEM_LIBS)

tests/t_crt: tests/t_crt.c crt.o mem.o bigint.o fatal.o crt.h mem.h tests/harness.h
	$(HIPC) $< -x none crt.o mem.o bigint.o fatal.o -o $@ -lgmp

tests/t_ntt: tests/t_ntt.c ntt.o ntt3.o bigint.o fatal.o ntt.h modarith.h tests/harness.h
	$(HIPC) $< -x none ntt.o ntt3.o bigint.o fatal.o -o $@ -lgmp

tests/t_alloc: tests/t_alloc.c ntt.o ntt3.o bigint.o mem.o fatal.o ntt.h mem.h tests/harness.h
	$(HIPC) $< -x none ntt.o ntt3.o bigint.o mem.o fatal.o -o $@ -lgmp -lpthread

tests/t_ntt2: tests/t_ntt2.c ntt2.o bigint.o fatal.o ntt2.h modarith2.h tests/harness.h
	$(HIPC) $< -x none ntt2.o bigint.o fatal.o -o $@ -lgmp

ref/gen_e: ref/gen_e.c tests/sha256.h
	gcc $(CFLAGS) -o $@ $< -lgmp -lm

tests/t_modarith: tests/t_modarith.c bigint.o fatal.o modarith.h tests/harness.h bigint.h
	$(HIPC) $< -x none bigint.o fatal.o -o $@ -lgmp

clean:
	rm -f $(REF) $(TESTS) $(OBJS) ecalc

.PHONY: all clean
