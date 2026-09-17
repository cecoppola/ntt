# ecalc - the paper's pipeline (PLAN.md 8).   module load rocm && make
ARCH   ?= gfx942
HIPCC  ?= hipcc
HIPFLAGS = -g -O3 --offload-arch=$(ARCH) -fopenmp -I. -I../bench
HIPC     = $(HIPCC) $(HIPFLAGS) -x hip
CFLAGS   = -O2 -Wall -fopenmp -I.

TESTS = tests/t_modarith tests/t_ntt tests/t_ntt2 tests/t_mul tests/t_crt tests/t_newton tests/t_bs tests/t_dec tests/t_verify tests/t_dist
OBJS  = ntt.o ntt2.o mem.o crt.o crt2.o comm_local.o comm_sim4.o ntt_dist.o bigint.o rns_mul.o newton.o binsplit.o todec.o verify.o
REF   = ref/gen_e

all: $(REF) $(OBJS) $(TESTS) ecalc

ecalc: ecalc.c $(OBJS) rns_mul.h newton.h binsplit.h todec.h verify.h mem.h ntt.h
	$(HIPC) $< -x none $(OBJS) -o $@

ntt.o: ntt.c ntt.h modarith.h
	$(HIPC) -c -o $@ $<
ntt2.o: ntt2.c ntt2.h modarith2.h
	$(HIPC) -c -o $@ $<
rns_mul.o: rns_mul.c rns_mul.h ntt.h mem.h crt.h bigint.h modarith.h
	$(HIPC) -c -o $@ $<
mem.o: mem.c mem.h
	$(HIPC) -c -o $@ $<
comm_local.o: comm_local.c comm.h
	$(HIPC) -c $< -o $@

comm_sim4.o: comm_sim4.c comm.h
	$(HIPC) -c $< -o $@

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
binsplit.o: binsplit.c binsplit.h bigint.h rns_mul.h mem.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
newton.o: newton.c newton.h bigint.h rns_mul.h mem.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<
bigint.o: bigint.c bigint.h
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<

tests/t_mul: tests/t_mul.c $(OBJS) rns_mul.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp

tests/t_newton: tests/t_newton.c $(OBJS) newton.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp

tests/t_dist: tests/t_dist.c $(OBJS) ntt_dist.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp

tests/t_bs: tests/t_bs.c $(OBJS) binsplit.h tests/harness.h tests/sha256.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp -lm

tests/t_dec: tests/t_dec.c $(OBJS) todec.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp

tests/t_verify: tests/t_verify.c verify.o binsplit.o bigint.o rns_mul.o ntt.o mem.o crt.o newton.o verify.h tests/harness.h
	$(HIPC) $< -x none $(OBJS) -o $@ -lgmp

tests/t_crt: tests/t_crt.c crt.o mem.o bigint.o crt.h mem.h tests/harness.h
	$(HIPC) $< -x none crt.o mem.o bigint.o -o $@ -lgmp

tests/t_ntt: tests/t_ntt.c ntt.o bigint.o ntt.h modarith.h tests/harness.h
	$(HIPC) $< -x none ntt.o bigint.o -o $@ -lgmp

tests/t_ntt2: tests/t_ntt2.c ntt2.o bigint.o ntt2.h modarith2.h tests/harness.h
	$(HIPC) $< -x none ntt2.o bigint.o -o $@ -lgmp

ref/gen_e: ref/gen_e.c tests/sha256.h
	gcc $(CFLAGS) -o $@ $< -lgmp -lm

tests/t_modarith: tests/t_modarith.c bigint.o modarith.h tests/harness.h bigint.h
	$(HIPC) $< -x none bigint.o -o $@ -lgmp

clean:
	rm -f $(REF) $(TESTS) $(OBJS) ecalc

.PHONY: all clean
