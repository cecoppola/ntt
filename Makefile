# module load rocm && make          (or just ./suite, which does both)
ARCH   ?= gfx942
HIPCC  ?= hipcc
# Sources are C.  -x hip selects the HIP language for device code.
HIPFLAGS = -x hip -O3 --offload-arch=$(ARCH) -fopenmp -Ibench

BENCH = bench/01_butterfly bench/02_capacity bench/03_fabric bench/06_ntt_opt bench/07_ntt_tw bench/09_logic bench/10_lds bench/11_fabric_ntt bench/12_ntt_inv bench/13_multiply2 bench/14_sustained bench/15_barrett_f64 bench/17_hostreg bench/19_peer_gather bench/21_alloc bench/22_clock bench/16_ntt_tile bench/18_staging bench/20_cpu bench/23_d2h bench/mem/infcache bench/arith/mfma bench/system/launch bench/mem/sweep bench/fabric/p2p bench/arith/occupancy bench/lds/occupancy bench/system/alloc bench/mem/stride bench/lds/xchg bench/kernel/rowN bench/fabric/poolread bench/fabric/cpuhbm
OLD = bench/old/04_ntt_lds bench/old/05_ntt_reg bench/old/08_multiply
TESTS = tests/t_params

all: $(BENCH) $(TESTS)

ecalc:
	$(MAKE) -C ecalc

old: $(OLD)

bench/kernel/rowN: bench/kernel/rowN.c ecalc/ntt.o
	$(HIPCC) $(HIPFLAGS) $< -x none ecalc/ntt.o -o $@

bench/%: bench/%.c bench/common_ntt.h bench/ntt_kernels.h
	$(HIPCC) $(HIPFLAGS) -o $@ $<

bench/20_cpu: bench/20_cpu.c bench/common_ntt.h
	$(HIPCC) $(HIPFLAGS) -o $@ $< -lgmp

tests/%: tests/%.c
	gcc -O2 -Wall -o $@ $< -lgmp -lm

# make isa B=15_barrett_f64 K=k_rate   -> instruction counts per kernel (isa/)
isa:
	mkdir -p isa && cd isa && $(HIPCC) $(HIPFLAGS) --save-temps -o $(B) ../bench/$(B).c $(if $(filter 20_cpu,$(B)),-lgmp,) 2>/dev/null; cd .. && python3 isa.py isa/$(B)-hip-amdgcn-amd-amdhsa-gfx942.s $(K)

clean:
	rm -f $(BENCH) $(TESTS) $(OLD)

.PHONY: all clean isa old ecalc
