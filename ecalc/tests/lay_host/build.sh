#!/bin/bash
# S45: host-only build of the layered communicator + a many-rank TCP harness (tests/lay_host/t_lay.cc).  comm_layered.c is copied with
# its three kernel launches rewritten (perl) so g++ takes it; the repository source is not touched.  Usage: build.sh [outdir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); EC=$(cd "$HERE/../.." && pwd); OUT=${1:-$HERE/out}; mkdir -p "$OUT"
perl -pe 's/(\w+)<<<.*?, 256, 0, s>>>\(/$1_h(/' "$EC/comm_layered.c" > "$OUT/comm_layered_host.cc"
F="-O2 -g -DCOMM_HOST_ONLY -DEC_FATAL_NO_HIP -I$EC -I$HERE"
gcc $F -c "$EC/comm_tcp.c" -o "$OUT/comm_tcp.o"
gcc $F -c "$EC/comm_util.c" -o "$OUT/comm_util.o"
gcc $F -c "$EC/fatal.c" -o "$OUT/fatal.o"
g++ $F -include "$HERE/hip_host_shim.h" -fpermissive -w -c "$OUT/comm_layered_host.cc" -o "$OUT/comm_layered_host.o"
g++ $F -include "$HERE/hip_host_shim.h" -fpermissive -w -c "$HERE/t_lay.cc" -o "$OUT/t_lay.o"
g++ -o "$OUT/t_lay" "$OUT/t_lay.o" "$OUT/comm_layered_host.o" "$OUT/comm_tcp.o" "$OUT/comm_util.o" "$OUT/fatal.o" -lpthread
echo built "$OUT/t_lay"
