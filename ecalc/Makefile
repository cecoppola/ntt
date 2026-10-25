# ecalc - the pipeline (ALGORITHM.md).   module load rocm && make
ARCH   ?= gfx942
HIPCC  ?= hipcc
HIPFLAGS = -g -O3 --offload-arch=$(ARCH) -fopenmp -I. -I../bench
HIPC     = $(HIPCC) $(HIPFLAGS) -x hip
CFLAGS   = -O2 -Wall -fopenmp -I.
ifeq ($(origin CC),default)
CC       = gcc                       # gcc unless CC is given (make's built-in `cc` is Cray's wrapper on aac7)
endif
# GMP_HOME=<prefix> points the tests, tools and ref/gen_e at a GMP built from source (aac7 has libgmp.so.10 but no headers).
GMP_HOME   ?=
ifneq ($(GMP_HOME),)
HIPFLAGS += -I$(GMP_HOME)/include
CFLAGS   += -I$(GMP_HOME)/include
GMP_LIBS  = -L$(GMP_HOME)/lib -lgmp -Wl,-rpath,$(GMP_HOME)/lib
else
GMP_LIBS  = -lgmp
endif
# libfabric (OFI=1 automatic where $(LIBFABRIC)/include/rdma/fabric.h exists: aac7's /opt/cray/libfabric/2.3.1) is the data plane of XSHMEM=ofi.
LIBFABRIC ?= $(if $(CRAY_LIBFABRIC_PREFIX),$(CRAY_LIBFABRIC_PREFIX),/opt/cray/libfabric/2.3.1)
OFI ?= $(if $(wildcard $(LIBFABRIC)/include/rdma/fabric.h),1,0)
ifeq ($(OFI),1)
OFI_CFLAGS = -I$(LIBFABRIC)/include
OFI_LIBS   = -L$(LIBFABRIC)/lib64 -lfabric -Wl,-rpath,$(LIBFABRIC)/lib64
endif

TESTS = tests/t_arith tests/t_ntt tests/t_mul tests/t_crt tests/t_newton tests/t_bs tests/t_verify tests/t_dist tests/t_dbig tests/t_out tests/t_mn_grid tests/t_alloc tests/t_strategy tests/t_cap tests/t_seed tests/t_patch tests/t_p24 tests/t_crtk tests/t_vpe
OBJS  = ntt.o ntt3.o mem.o crt.o comm_xgmi.o comm_copy.o comm_layered.o comm_util.o ntt_dist.o mn.o mn_out.o bigint.o rns_mul.o rns_dist.o rns_local.o dbig.o newton_db.o newton.o binsplit.o verify.o mn_plan.o spill.o memsample.o fatal.o segv_trace.o p24_crt.o config_line.o vpe.o
REF   = ref/gen_e
HDRS  = $(wildcard *.h)                 # every object and test depends on every header (one rule instead of a dependency line per file)

all: $(REF) $(OBJS) $(TESTS) ecalc tools

# the packed digit converter / checker (default output) is part of every build
tools:
	$(MAKE) -s -C ../tools

ecalc: ecalc.c $(OBJS) $(HDRS)
	$(HIPC) $< -x none $(OBJS) -o $@ $(XSHMEM_LIBS)

# objects by compile line: hipcc -x hip with -o first / -o last, and the host-C objects
OBJ_HIP1 = config_line.o ntt.o ntt3.o rns_mul.o rns_local.o rns_dist.o p24_crt.o mn_plan.o dbig.o spill.o fatal.o segv_trace.o newton_db.o mem.o vpe.o
OBJ_HIP2 = comm_layered.o mn.o mn_out.o comm_copy.o comm_util.o comm_xgmi.o ntt_dist.o
OBJ_C = memsample.o crt.o verify.o binsplit.o newton.o bigint.o
$(OBJ_HIP1): %.o: %.c $(HDRS)
	$(HIPC) -c -o $@ $<
$(OBJ_HIP2): %.o: %.c $(HDRS)
	$(HIPC) -c $< -o $@
$(OBJ_C): %.o: %.c $(HDRS)
	$(HIPCC) -O3 -fopenmp -I. -x c -c -o $@ $<

# tests linked with OBJS (and the engine), one rule; the rest below
LINK_TESTS = tests/t_arith tests/t_p24 tests/t_crtk tests/t_dbig tests/t_out tests/t_patch tests/t_mn_grid tests/t_vpe tests/t_mul tests/t_newton tests/t_strategy tests/t_verify tests/t_seed tests/t_bs
$(LINK_TESTS): %: %.c $(OBJS) $(HDRS) tests/harness.h tests/tref.h tests/sha256.h
	$(HIPC) $< -x none $(OBJS) -o $@ $(GMP_LIBS) -lm $(XSHMEM_LIBS)

# t_dist drives the test-only communicators (tests/comm_local.c, tests/comm_sim4.c: one rank / four synthetic ranks in one APU)
tests/t_dist: tests/t_dist.c tests/comm_local.c tests/comm_sim4.c $(OBJS) $(HDRS) tests/harness.h
	$(HIPC) -fopenmp $< tests/comm_local.c tests/comm_sim4.c -x none $(OBJS) -o $@ $(GMP_LIBS) -lm $(XSHMEM_LIBS)

# tests with fewer objects: the plane cap (host only), the CRT, the transforms, the allocator
tests/t_cap: tests/t_cap.c tests/harness.h
	gcc $(CFLAGS) -o $@ $< $(GMP_LIBS) -lm
tests/t_crt: tests/t_crt.c crt.o mem.o bigint.o fatal.o $(HDRS) tests/harness.h
	$(HIPC) $< -x none crt.o mem.o bigint.o fatal.o -o $@ $(GMP_LIBS)
tests/t_ntt: tests/t_ntt.c ntt.o ntt3.o bigint.o fatal.o $(HDRS) tests/harness.h
	$(HIPC) $< -x none ntt.o ntt3.o bigint.o fatal.o -o $@ $(GMP_LIBS)
tests/t_alloc: tests/t_alloc.c ntt.o ntt3.o bigint.o mem.o fatal.o $(HDRS) tests/harness.h
	$(HIPC) $< -x none ntt.o ntt3.o bigint.o mem.o fatal.o -o $@ $(GMP_LIBS) -lpthread

# built on demand (target_kit.sh), not in TESTS
tests/t_edge: tests/t_edge.c     # the memory-edge probe
	$(HIPC) $< -o $@
tests/t_nodechk: tests/t_nodechk.c     # target_kit.sh stage nodechk: per-node meminfo + 1 GB host->device upload per APU
	$(HIPC) $< -o $@

ref/gen_e: ref/gen_e.c tests/sha256.h
	gcc $(CFLAGS) -o $@ $< $(GMP_LIBS) -lm

clean:
	rm -f $(REF) $(TESTS) $(OBJS) ecalc

.PHONY: all clean tools

# ---- XSHMEM: the communication layer behind xshmem.h (the only multi-node transport).  On by default wherever libfabric (OFI=1) and the PMI2
# header are found, else 0 (the build without the engine: it still builds, COMM_TRANSPORT=xshmem is then a fatal at start).
#   make                                             XSHMEM=ofi (path 1): libfabric cxi backend (xshmem_ofi.c, xshmem_boot.c PMI2, pe_geo.c)
#   make XSHMEM=cray                                 (path 2): Cray SHMEM backend (xshmem_cray.h, static inline); aac7: `module load cray-dsmml cray-openshmemx`
#   make XSHMEM=ofi tests/t_xshmem tests/t_pe_geo    the engine tests (build with -B when switching XSHMEM)
# XSHMEM_CRAY_DEFS = -D selectors of xshmem_cray.h (XSHMEM_CRAY_HEAP, _SIGSET, _WAIT, _TEST, _PUTSIG_SPLIT).  XTEST_OBJS = extra objects for tests/t_xshmem.
PMI_HOME ?= /opt/cray/pe/pmi/default
XSHMEM ?= $(if $(and $(filter 1,$(OFI)),$(wildcard $(PMI_HOME)/include/pmi2.h)),ofi,0)
XSHMEM_CRAY_DEFS ?=
XTEST_OBJS ?=
XSHMEM_LIBS =
ifeq ($(XSHMEM),ofi)
ifneq ($(OFI),1)
$(error XSHMEM=ofi needs libfabric headers: OFI=1 (LIBFABRIC=$(LIBFABRIC)))
endif
XSHMEM_CFLAGS = -DXSHMEM_OFI -DUSE_HIP $(OFI_CFLAGS) -I$(PMI_HOME)/include
XSHMEM_LIBS   = $(OFI_LIBS) -L/opt/cray/pe/lib64 -lpmi2 -Wl,-rpath,/opt/cray/pe/lib64 -lpthread
XOBJS         = xshmem_ofi.o xshmem_boot.o pe_geo.o
else ifeq ($(XSHMEM),cray)
CRAY_SMA   ?= $(if $(CRAY_OPENSHMEMX_PREFIX),$(CRAY_OPENSHMEMX_PREFIX),/opt/cray/pe/sma/11.8.0/ofi/sma)
CRAY_DSMML ?= $(if $(CRAY_DSMML_PREFIX),$(CRAY_DSMML_PREFIX),/opt/cray/pe/dsmml/0.3.1/dsmml)
XSHMEM_CFLAGS = -DXSHMEM_CRAY -DUSE_HIP $(XSHMEM_CRAY_DEFS) -I$(CRAY_SMA)/include -I$(CRAY_DSMML)/include
XSHMEM_LIBS   = -L$(CRAY_SMA)/lib64 -L$(CRAY_DSMML)/lib -L/opt/cray/pe/lib64 -lsma -lpmi -lpmi2 -ldsmml -Wl,-rpath,$(CRAY_SMA)/lib64 -Wl,-rpath,$(CRAY_DSMML)/lib -Wl,-rpath,/opt/cray/pe/lib64 -lpthread
XOBJS         = pe_geo_nb.o
else ifneq ($(XSHMEM),0)
$(error XSHMEM must be 0, ofi or cray)
endif

ifneq ($(XSHMEM),0)
xshmem_ofi.o: xshmem_ofi.c $(HDRS)
	$(HIPC) $(XSHMEM_CFLAGS) -c $< -o $@
xshmem_boot.o: xshmem_boot.c $(HDRS)
	$(HIPC) $(XSHMEM_CFLAGS) -c $< -o $@
pe_geo.o: pe_geo.c $(HDRS)
	$(HIPC) $(XSHMEM_CFLAGS) -c $< -o $@
pe_geo_nb.o: pe_geo.c $(HDRS)
	$(HIPC) $(XSHMEM_CFLAGS) -DPE_GEO_NOBOOT -c $< -o $@
tests/t_xshmem: tests/t_xshmem.c $(XOBJS) $(HDRS) tests/harness.h
	$(HIPC) $(XSHMEM_CFLAGS) $< -x none $(XOBJS) $(XTEST_OBJS) -o $@ $(XSHMEM_LIBS)
ifeq ($(XSHMEM),ofi)
tests/t_pe_geo: tests/t_pe_geo.c pe_geo.c $(HDRS) xshmem_boot.o
	$(HIPC) $(XSHMEM_CFLAGS) -I. $< pe_geo.c -x none xshmem_boot.o -o $@ $(XSHMEM_LIBS)
endif
# every binary that links OBJS (ecalc and the tests) also links the engine (comm_xshmem.c) and the backend; mn.o is compiled with -DXSHMEM_ENGINE
# (without it COMM_TRANSPORT=xshmem is a fatal at start).  XSHMEM=cray builds the SAME engine, XOBJS = pe_geo_nb.o: the backend is the inline
# xshmem_cray.h and -lsma.  Rebuild with -B when switching XSHMEM.
comm_xshmem.o: comm_xshmem.c $(HDRS)
	$(HIPC) $(XSHMEM_CFLAGS) -DXSHMEM_ENGINE -c $< -o $@
OBJS += comm_xshmem.o $(XOBJS)
mn.o: comm_xshmem.o $(XOBJS)
mn.o: HIPFLAGS += -DXSHMEM_ENGINE
else
tests/t_xshmem tests/t_pe_geo:
	@echo "set XSHMEM=ofi or XSHMEM=cray (XSHMEM=0 here: libfabric or PMI2 not found)"; false
endif
