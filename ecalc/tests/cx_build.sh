#!/bin/bash
# cx_build.sh (Phase 15 CX): builds tests/cx_grid as the Makefile builds t_mn_grid (no Makefile edit); run in ecalc/ after make
set -e
cd "$(dirname "$0")/.."
SHMEM_LIBS=""; command -v oshcc > /dev/null 2>&1 && SHMEM_LIBS="$(oshcc --showme:link) -lopen-rte -lopen-pal"
# PC (Phase 15 Batch 3): + p24_crt.o (P24)
OBJS="ntt.o ntt3.o ntt2.o mem.o crt.o crt2.o comm_local.o comm_sim4.o comm_xgmi.o comm_tcp.o comm_shmem.o comm_layered.o comm_util.o ntt_dist.o mn.o mn_out.o bigint.o rns_mul.o rns_dist.o dbig.o newton_db.o newton.o binsplit.o todec.o verify.o mn_plan.o spill.o memsample.o fatal.o p24_crt.o"
hipcc -g -O3 --offload-arch=gfx942 -fopenmp -I. -I../bench -x hip tests/cx_grid.c -x none $OBJS -o tests/cx_grid -lgmp $SHMEM_LIBS
echo "built tests/cx_grid"
