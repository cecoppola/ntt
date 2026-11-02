#!/bin/bash
# digcmp.sh <outfile> <ASCII reference>: a run's digits (the file, or its part files <outfile>.part*) against the reference;
# prints identical / DIFFERS.  Packed parts (ECALC_OUT_PACKED nonzero, the default since 2026-10-12; the 8-byte form of =1 was removed, its files are still read: header "ECPACK18") are compared by
# tools/unpack_digits --cmp without an ASCII copy; ASCII files with O_DIRECT reads (nothing left in the page cache).
# A reference longer than the run (a smaller run against a bigger reference) is compared by its prefix of the run's length
# (the run's final newline is not matched against a reference digit; unpack_digits --prefix for packed parts) and prints
# "identical (prefix of the reference)"; equal lengths behave as before, a run longer than the reference is DIFFERS.
f=$1 ref=$2; here=$(cd "$(dirname "$0")" && pwd)
P=$(ls "$f".part* 2>/dev/null | sort); [ -n "$P" ] || P=$f
first=$(echo "$P" | head -1); last=$(echo "$P" | tail -1)
rs=$(stat -L -c %s "$ref" 2>/dev/null || echo 0)
if [ "$(head -c 8 "$first" 2>/dev/null)" = ECPACK18 ]; then
  n=$(od -An -tu8 -j56 -N8 "$first" 2>/dev/null | tr -d ' ')   # header d_out: the run's ASCII is "2." + d_out digits + "\n"
  pre=; [ -n "$n" ] && [ "$rs" -gt $((n + 3)) ] && pre=--prefix
  "$here/../tools/unpack_digits" -q --cmp "$ref" $pre $P > /dev/null 2>&1 && { [ -n "$pre" ] && echo "identical (prefix of the reference)" || echo identical; } || echo DIFFERS
else
  n=0; for p in $P; do n=$((n + $(stat -L -c %s "$p" 2>/dev/null || echo 0))); done
  if [ "$rs" -gt "$n" ] && [ "$n" -gt 0 ]; then
    [ "$(tail -c1 "$last" | od -An -c | tr -d ' ')" = '\n' ] && cmp -s <(for p in $P; do dd if=$p iflag=direct bs=64M status=none; done | head -c $((n - 1))) <(dd if="$ref" iflag=direct bs=64M status=none | head -c $((n - 1))) && echo "identical (prefix of the reference)" || echo DIFFERS
  else
    cmp -s <(for p in $P; do dd if=$p iflag=direct bs=64M status=none; done) <(dd if="$ref" iflag=direct bs=64M status=none) && echo identical || echo DIFFERS
  fi
fi
