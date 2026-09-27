#!/usr/bin/env python3
"""L8 (Phase 15, throwaway): candidate NTT primes for the 5 2^k / 15 2^k / 7 2^k length families.

The code's size bound (modarith.h, ntt.c mm_raw): 2^51 < p (mu115 = floor(2^115 / p) must fit 64 bits; canon64 shifts by 51)
and 1.51 p + 2^51 < 2^53 (the reduced-correction modmul's exact fma), i.e. p < 0.99338 2^52.  A plane of 2^40 points (the
576-node cap) needs 2^40 | p - 1.  p = k 2^40 + 1 with 2048 < k < 4068 covers every such prime (c 2^44 + 1 is k = 16 c).
Prints: today's set, every prime with 15 | k (3, 5 | p - 1) and with 105 | k (3, 5, 7), their 2-adic order, the smallest
primitive root, and the best 3- and 4-prime products with the CRT bound in terms (nterms (10^18 - 1)^2 < prod p)."""
import math, itertools

def is_prime(n):
    if n < 2: return False
    for q in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % q == 0: return n == q
    d, s = n - 1, 0
    while d % 2 == 0: d //= 2; s += 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        x = pow(a, d, n)
        if x in (1, n - 1): continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1: break
        else: return False
    return True

def factors(n):
    f, d = set(), 2
    while d * d <= n:
        while n % d == 0: f.add(d); n //= d
        d += 1
    if n > 1: f.add(n)
    return sorted(f)

def prim_root(p):
    fs = factors(p - 1)
    for g in range(2, 200):
        if all(pow(g, (p - 1) // q, p) != 1 for q in fs): return g

def v2(n):
    s = 0
    while n % 2 == 0: n //= 2; s += 1
    return s

LO, HI = 2 ** 51, int(0.99338 * 2 ** 52)
D = (10 ** 18 - 1) ** 2
TODAY = [240 * 2 ** 44 + 1, 216 * 2 ** 44 + 1, 207 * 2 ** 44 + 1, 147 * 2 ** 44 + 1]

def odd(p):
    m = p - 1
    while m % 2 == 0: m //= 2
    return m

def row(p):
    m = odd(p)
    return "%d = %d * 2^%d + 1  (p-1 odd part %d = %s; log2 p %.3f; g %d)" % (p, m, v2(p - 1), m, "*".join(map(str, factors(m))) or "1", math.log2(p), prim_root(p))

def bound(ps):
    P = math.prod(ps)
    return P, (P - 1) // D

print("size bound: %d < p < %d (2^51, 0.99338 2^52); 2^40 | p - 1" % (LO, HI))
print("\ntoday (modarith.h EC_PRIMES=1):")
for p in TODAY: print("  " + row(p), "prime" if is_prime(p) else "NOT PRIME")
P3, t3 = bound(TODAY[:3])
print("  three-prime product 2^%.3f, at most %d terms (2^%.3f)" % (math.log2(P3), t3, math.log2(t3)))
P4, t4 = bound(TODAY)
print("  four-prime product 2^%.3f, at most 2^%.3f terms" % (math.log2(P4), math.log2(t4)))

cand = [k * 2 ** 40 + 1 for k in range(2049, 4068) if is_prime(k * 2 ** 40 + 1) and LO < k * 2 ** 40 + 1 < HI]
for name, mod in (("3 | p-1 (today's family)", 3), ("15 | p-1 (5 2^k and 15 2^k)", 15), ("105 | p-1 (5, 7, 15 2^k)", 105), ("9 | p-1 (9 2^k; 45 = 9 5 for 45 2^k)", 9), ("45 | p-1", 45)):
    S = [p for p in cand if (p - 1) % mod == 0]
    S.sort(reverse=True)
    print("\n%s: %d primes with 2^40 | p - 1 in the bound" % (name, len(S)))
    for p in S[:12]: print("  " + row(p))
    if len(S) > 12: print("  ... (%d more, smaller)" % (len(S) - 12))
    if len(S) >= 3:
        b3 = S[:3]; P, t = bound(b3)
        print("  best three: product 2^%.3f, at most %d terms (2^%.3f); margin at 2^33 terms %.2f, at 2^35 %.2f, at 2^39 %.4f" % (math.log2(P), t, math.log2(t), t / 2 ** 33, t / 2 ** 35, t / 2 ** 39))
    if len(S) >= 4:
        b4 = S[:4]; P, t = bound(b4)
        print("  best four: product 2^%.3f, at most 2^%.3f terms; margin at 2^39 terms 2^%.1f" % (math.log2(P), math.log2(t), math.log2(t / 2 ** 39)))

print("\nThe 576 cap: a 2^40-point plane holds a piece of nc <= 2^40 limbs, nterms = min(pa, pb) <= 2^39.")
print("needed: prod p > 2^39 (10^18 - 1)^2 = 2^%.3f; three primes < 2^52 give at most 2^156.0: impossible for ANY three-prime set" % math.log2(2 ** 39 * D))
print("the target plan's largest piece (MN_PLAN_ONLY=4.25e13:576): 590277777779 + 472222222223 limbs, nterms 472222222223 = 2^%.3f" % math.log2(472222222223))
