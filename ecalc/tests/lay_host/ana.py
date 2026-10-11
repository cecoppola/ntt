import sys,re,glob
for f in sys.argv[1:]:
    rows=[l.split() for l in open(f) if l.startswith('T ')]
    if not rows: print(f,'no rows'); continue
    def col(name,off=1):
        return [int(r[r.index(name)+off]) for r in rows]
    pk=col('peak'); lv=col('live'); sy=col('pool_sym'); sa=col('pool_sym_after'); at=col('asym_top'); al=col('asym_low'); ag=col('asym_log'); bd=col('bad'); af=col('after')
    summ=open(f).read().strip().splitlines()[-1]
    print(f"{f.split('/')[-1]}: procs {len(rows)} bad {sum(bd)} peak(max/min) {max(pk)}/{min(pk)} live_end {max(lv)} pool_sym {max(sy)}/{min(sy)} after {sum(af)}+{sum(sa)} asym_top {sum(at)} asym_low {sum(al)} asym_log {sum(ag)} | {summ[-30:]}")
