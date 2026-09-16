#!/usr/bin/env python3
"""isa.py <file.s> [kernel-substring...]
Per kernel: VGPR/SGPR/occupancy from the metadata, instruction-class counts
for the whole kernel and for its innermost loop (largest block that ends in a
backward branch).  PLAN.md 7.1 'make isa'."""
import re, sys
src = open(sys.argv[1]).read()
want = sys.argv[2:]
classes = [("valu_total", r"^\s+v_"), ("f64", r"^\s+v_\w*_f64"), ("fma_f64", r"^\s+v_fma_f64"),
           ("mul_f64", r"^\s+v_mul_f64"), ("add_f64", r"^\s+v_add_f64"), ("floor/rndne_f64", r"^\s+v_(floor|rndne)_f64"),
           ("cmp", r"^\s+v_cmp"), ("cndmask", r"^\s+v_cndmask"), ("mad_u64", r"^\s+v_mad_u64_u32"),
           ("mul_hi/lo_u32", r"^\s+v_mul_(hi|lo)_u32"), ("mov_b32", r"^\s+v_mov_b32"), ("salu", r"^\s+s_(?!nop|waitcnt|cbranch|branch|endpgm)"),
           ("s_nop", r"^\s+s_nop"), ("waitcnt", r"^\s+s_waitcnt"), ("ds", r"^\s+ds_"), ("global/flat", r"^\s+(global|flat|buffer)_")]
def count(lines):
    return {n: sum(1 for l in lines if re.match(p, l)) for n, p in classes}
for m in re.finditer(r"^(\S+):\s*;\s*@\1\n(.*?)^\.Lfunc_end\d+:", src, re.S | re.M):
    name, body = m.group(1), m.group(2)
    if want and not any(w in name for w in want): continue
    lines = body.split("\n")
    meta = {}
    for k in ("NumVgprs", "NumSgprs", "Occupancy", "ScratchSize", "LDSByteSize"):
        mm = re.search(r";\s*%s:\s*(\d+)" % k, body)
        if mm: meta[k] = mm.group(1)
    # innermost loop: for each backward branch, the block from its target label
    best = []
    labels = {}
    for i, l in enumerate(lines):
        lm = re.match(r"^(\.LBB\d+_\d+):", l)
        if lm: labels[lm.group(1)] = i
        bm = re.match(r"^\s+s_cbranch_\w+\s+(\.LBB\d+_\d+)", l)
        if bm and bm.group(1) in labels:
            blk = lines[labels[bm.group(1)]:i + 1]
            if len(blk) > len(best): best = blk
    tot, loop = count(lines), count(best)
    print("== %s  vgpr %s sgpr %s occ %s lds %s scratch %s" % (name[:60], meta.get("NumVgprs"), meta.get("NumSgprs"),
          meta.get("Occupancy"), meta.get("LDSByteSize"), meta.get("ScratchSize")))
    print("   %-18s %8s %8s" % ("class", "kernel", "loop"))
    for n, _ in classes:
        print("   %-18s %8d %8d" % (n, tot[n], loop[n]))
    print("   (loop = %d lines)" % len(best))
