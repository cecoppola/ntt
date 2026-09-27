#!/bin/bash
# digcmp.sh <outfile> <ASCII reference>: a run's digits (the file, or its part files <outfile>.part*) against the reference;
# prints identical / DIFFERS.  Packed parts (ECALC_OUT_PACKED, the default since Phase 15: header "ECPACK18") are compared by
# tools/unpack_digits --cmp without an ASCII copy; ASCII files with O_DIRECT reads (nothing left in the page cache)
f=$1 ref=$2; here=$(cd "$(dirname "$0")" && pwd)
P=$(ls "$f".part* 2>/dev/null | sort); [ -n "$P" ] || P=$f
first=$(echo "$P" | head -1)
if [ "$(head -c 8 "$first" 2>/dev/null)" = ECPACK18 ]; then
  "$here/../tools/unpack_digits" -q --cmp "$ref" $P > /dev/null 2>&1 && echo identical || echo DIFFERS
else
  cmp -s <(for p in $P; do dd if=$p iflag=direct bs=64M status=none; done) <(dd if="$ref" iflag=direct bs=64M status=none) && echo identical || echo DIFFERS
fi
