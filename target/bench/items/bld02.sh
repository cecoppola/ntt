#!/bin/bash
# bld02 - path 2: make XSHMEM=cray (Cray SHMEM backend; run with ECALC_PE_PER_GPU=1 XSHMEM_LANES=2) into build/<sha7>-p2/; login node.
exec bash -l "$(dirname "${BASH_SOURCE[0]}")/_build.sh" p2
