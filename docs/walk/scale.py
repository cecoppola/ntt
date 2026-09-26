"""The shapes of the calculation at d = 1.4e11 digits (the largest verified single-node run, RESULTS §83 / results/R114.md):
every derived quantity quoted in docs/PAPER.md chapter A.  Exact where it follows from d (N, limb counts, levels, Newton
schedule, plane sizes); the measured times and memory are quoted from the run logs, not computed here."""
import math
LN10 = math.log(10); B_DIG = 18; SEED = 256; CAP = 2**31
def log10fact(n): return math.lgamma(n + 1) / LN10
def e_terms(d):
    lo, hi = 1, 2
    while log10fact(hi) < d + 50: hi *= 2
    while lo < hi:
        m = (lo + hi) // 2
        lo, hi = (lo, m) if log10fact(m) >= d + 50 else (m + 1, hi)
    return lo
def limbs(a, b): return (log10fact(b) - log10fact(a)) / B_DIG
d = 14 * 10**10
N = e_terms(d); nQ = limbs(0, N); nA = nQ + d / B_DIG; k = math.ceil(nA) - math.ceil(nQ) + 1
lev = math.ceil(math.log2(N / SEED))
print(f"d = {d:.3e}: N = {N:,}  log10 N! = {log10fact(N):.6e}")
print(f"Q: {nQ:.4e} limbs = {nQ*8/1e9:.1f} GB;  A: {nA:.4e} limbs = {nA*8/1e9:.1f} GB;  k = {k:.4e} limbs")
print(f"seeds: {math.ceil(N/SEED):,} spans of {SEED} terms; merge levels above the seeds: {lev}")
for t in range(5):
    m = 2**t; big = limbs(N - N // m, N) if m > 1 else nQ
    print(f" top-{t}: {m} nodes, largest Q {big:.3e} limbs")
for t in (1, 2, 3):
    m = 2**t; s = limbs(N - N // m, N)
    print(f"  merge producing level top-{t-1}: products about {s:.3e} x {s:.3e} -> {2*s:.3e} limbs = {2*s/CAP:.2f} x the 2^31 cap")
# Newton schedule
j, ch = 2, []
while j < k:
    jn = int(k)
    while (jn + 1) // 2 > j: jn = (jn + 1) // 2
    ch.append(jn); j = jn
print(f"Newton: {len(ch)} steps; first {ch[:5]}, last {[f'{x:.3e}' for x in ch[-3:]]}")
n = CAP; lg = 31
print(f"plane 2^31 points x 8 B = {n*8/1e9:.2f} GB per prime; 3 primes = {3*n*8/1e9:.1f} GB per operand")
print(f"butterflies per 2^31 transform: n/2 log2 n = {n//2*lg:.3e}; per product (2 fwd + 1 inv) x 3 primes = {3*3*n//2*lg:.3e}")
print(f"all-to-all per 2^31 product over 4 APUs: 3 x 3 x 3/4 x {n*8/1e9:.2f} GB = {3*3*0.75*n*8/1e9:.0f} GB")
print(f"coefficient bound: n (B-1)^2 = {n*(10**18-1)**2:.3e}  vs  p0 p1 p2 = {4222124650659841*3799912185593857*3641582511194113:.3e}")
print(f"output: {d+1:.4e} characters = {(d+2)/1e9:.0f} GB")
