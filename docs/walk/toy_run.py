"""The whole ecalc calculation on a small input, with ecalc's own parameters (base 10^18 limbs, the three
primes c*2^44+1, their roots, the Newton step of newton.c, the division of newton_divmod).  Prints every
intermediate value quoted in docs/PAPER.md, "Walk-through".  Pure Python; run: python3 docs/walk/toy_run.py"""
import math
B = 10**18
P3 = [4222124650659841, 3799912185593857, 3641582511194113]      # ec_P[0..2]
W33 = [1664894315601502, 1343624525396189, 3536920527846901]     # order 2^33
W3X33 = [2693318777493321, 1846280463207194, 633252398701382]    # order 3*2^33

def limbs(x):
    out = []
    while x: out.append(x % B); x //= B
    return out or [0]
def val(l): return sum(v * B**i for i, v in enumerate(l))

def root(i, n):                       # a primitive n-th root mod p_i, n = 2^k or 3*2^k, n | 3*2^33
    p = P3[i]
    if (1 << 33) % n == 0: return pow(W33[i], (1 << 33) // n, p)
    return pow(W3X33[i], (3 << 33) // n, p)
def length(m):                        # the smallest admissible transform length >= m
    return min(L for L in [2**k for k in range(0, 40)] + [3 * 2**k for k in range(0, 40)] if L >= m)
def dft(x, w, p): return [sum(x[j] * pow(w, j * k, p) for j in range(len(x))) % p for k in range(len(x))]

def mul(a, b, show=False):            # a*b as limb lists: RNS-NTT over 3 primes, Garner, carry
    la, lb = len(a), len(b); n = length(la + lb - 1)
    res = []
    for i, p in enumerate(P3):
        w = root(i, n)
        A = dft(a + [0] * (n - la), w, p); Bt = dft(b + [0] * (n - lb), w, p)
        C = [x * y % p for x, y in zip(A, Bt)]
        c = [x * pow(n, -1, p) % p for x in dft(C, pow(w, -1, p), p)]
        res.append(c)
        if show and i == 0: print("  prime p0: n =", n, "\n   a^ =", A, "\n   b^ =", Bt, "\n   c^ =", C, "\n   c mod p0 =", c)
    p0, p1, p2 = P3; coef = []
    for r0, r1, r2 in zip(*res):      # Garner
        v0 = r0; v1 = (r1 - v0) * pow(p0, -1, p1) % p1
        v2 = ((r2 - v0) * pow(p0, -1, p2) - v1) * pow(p1, -1, p2) % p2
        coef.append(v0 + v1 * p0 + v2 * p0 * p1)
    if show: print("  residues of coef 1:", [r[1] for r in res], "\n  Garner coefs:", coef)
    out, carry = [], 0
    for c in coef + [0, 0, 0]:
        t = c + carry; out.append(t % B); carry = t // B
    while len(out) > 1 and out[-1] == 0: out.pop()
    assert val(out) == val(a) * val(b)
    return out

def e_terms(d):                       # smallest N with log10 N! >= d + 50, by bisection on lgamma
    f = lambda m: math.lgamma(m + 1) / math.log(10) >= d + 50
    lo, hi = 1, 2
    while not f(hi): hi *= 2
    while lo < hi:
        m = (lo + hi) // 2
        lo, hi = (lo, m) if f(m) else (m + 1, hi)
    return lo

def run(d, span):
    N = e_terms(d); print(f"d = {d}: N = {N} terms, log10 N! = {math.lgamma(N+1)/math.log(10):.2f}")
    # seeds: P(a,b), Q(a,b) over spans of `span` terms, by the recurrence term by term
    nodes = []
    for a in range(0, N, span):
        b = min(a + span, N); P, Q = 0, 1
        for k in range(a + 1, b + 1): P, Q = P * k + 1, Q * k           # P(a,k) = P(a,k-1) k + 1, Q(a,k) = Q(a,k-1) k
        nodes.append((a, b, P, Q))
    lvl = 0
    while True:
        print(f" level {lvl}: {len(nodes)} nodes; Q limbs " + " ".join(str(len(limbs(Q))) for a, b, P, Q in nodes))
        if len(nodes) == 1: break
        nxt = []
        for i in range(0, len(nodes) - 1, 2):
            (a, m, P1, Q1), (_, b, P2, Q2) = nodes[i], nodes[i + 1]
            last = len(nodes) in (2, 3) and i == 0
            if last: print(f" merge [{a},{m}) + [{m},{b}): P1*Q2, limbs", len(limbs(P1)), "x", len(limbs(Q2)), " P1 =", limbs(P1), " Q2 =", limbs(Q2))
            P1Q2 = val(mul(limbs(P1), limbs(Q2), show=last)); Q1Q2 = val(mul(limbs(Q1), limbs(Q2)))
            nxt.append((a, b, P1Q2 + P2, Q1Q2))
        if len(nodes) % 2: nxt.append(nodes[-1])                         # odd node: copied up
        nodes = nxt; lvl += 1
    _, _, P, Q = nodes[0]
    print(" P digits", len(str(P)), "Q digits", len(str(Q)), " Q = N! :", Q == math.factorial(N))
    A = 10**d * (P + Q); Al, Ql = limbs(A), limbs(Q)
    print(" A limbs", len(Al), " Q limbs", len(Ql))
    na, nq = len(Al), len(Ql); k = na - nq + 1
    # seed (newton_seed_top): r = B^(top+2) / (top limbs of Q, rounded up), j = 2
    top = min(4, nq); qt = val(Ql[nq - top:]) + (1 if top < nq else 0)
    r = B**(top + 2) // qt; j = 2
    print(f" Newton: k = {k}; seed r has {len(limbs(r))} limbs (j = 2); r/B^(nq+2) * Q = {r * Q / B**(nq + 2):.20f}")
    while j < k:
        jn = k
        while (jn + 1) // 2 > j: jn = (jn + 1) // 2
        take = min(2 * j + 2, nq); Qt = val(Ql[nq - take:])
        t1 = Qt * r; u = t1 // B**(take - j) if j <= take else t1 * B**(j - take)
        dd = B**(2 * j) - u; corr = r * abs(dd) // B**j
        r2 = r * B**j + (corr if dd >= 0 else -corr)
        if jn < 2 * j: r2 //= B**(2 * j - jn)
        err = abs(r2 - B**(nq + jn) // Q)
        print(f"  j {j} -> {jn}: take {take}, |d| has {len(limbs(abs(dd)))} limbs, d/B^(2j) = {dd / B**(2*j):.3e}, error after = {err} units")
        r, j = r2, jn
    mu = r; exact = B**(nq + k) // Q
    print(" mu - floor(B^(nq+k)/Q) =", mu - exact)
    Ah = A // B**(nq - 1); X = Ah * mu // B**(k + 1)
    ups = downs = 0
    while X * Q > A: X -= 1; downs += 1
    while A - X * Q >= Q: X += 1; ups += 1
    R = A - X * Q
    print(f" X0 corrections: down {downs}, up {ups};  X = {X}")
    print(" X == floor(10^d e)? digits:", str(X)[:1] + "." + str(X)[1:])
    q = (1 << 62) + 135                                                  # the first T1 modulus
    lhs = (pow(10, d, q) * ((P + Q) % q)) % q; rhs = (X % q * (Q % q) + R % q) % q
    print(" T1 mod 2^62+135:", lhs, rhs, lhs == rhs, "  R/Q =", R / Q)

if __name__ == "__main__":
    run(200, 16)
    # the real run's shapes: 4 x 10^10 digits
    d = 4 * 10**10; N = e_terms(d); lg = lambda a, b: (math.lgamma(b + 1) - math.lgamma(a + 1)) / math.log(10) / 18
    print(f"\nd = 4e10: N = {N}, Q = {lg(0, N):.4g} limbs, A = {lg(0,N) + d/18:.4g} limbs, levels above 256-term seeds = {math.ceil(math.log2(N/256))}")
    for lev in range(4):
        m = 2**lev; print(f" level top-{lev}: {m} nodes, Q of the largest = {lg(N - N//m, N):.4g} limbs")
