#!/usr/bin/env python3
"""mpb15_lines.py <k> <log>... (Phase 15 MPB): the RNS_VERBOSE product lines of a run under ECALC_NP=auto ECALC_NP_AUTO_MIN=1 with
ECALC_NP_AUTO_TERMS=k, checked against the rule: a product of na x nb limbs runs four primes iff min(na, nb) > k.  Parses the
dist_mn lines ("na + nb[ + x] limbs", ", N primes") and the B form's dist lines ("= na x nb, N primes"); the C form's size-1 line
names only nc (counted, not checked).  Prints the counts and the products moved to three (min <= k < na + nb); exit 1 on a line
against the rule."""
import re, sys
k = int(sys.argv[1]); bad = n3 = n4 = moved = nochk = 0; ex = None
MN = re.compile(r'dist_mn node \d+: .*?, ([34]) primes .*?; (\d+) \+ (\d+)( \+ x)? limbs at')
DB = re.compile(r'^dist \S+ \S+ \((\d+) limbs = (\d+) x (\d+), ([34]) primes\)')
DC = re.compile(r'^dist .*\(\d+ limbs, ([34]) primes\)')
for fn in sys.argv[2:]:
    for l in open(fn, errors='replace'):
        m = MN.search(l)
        if m: np_, na, nb = int(m.group(1)), int(m.group(2)), int(m.group(3))
        else:
            m = DB.search(l)
            if m: np_, na, nb = int(m.group(4)), int(m.group(2)), int(m.group(3))
            else:
                m = DC.search(l)
                if m: nochk += 1; n3 += m.group(1) == '3'; n4 += m.group(1) == '4'
                continue
        want = 4 if min(na, nb) > k else 3
        n3 += np_ == 3; n4 += np_ == 4
        if np_ != want: bad += 1; print('AGAINST THE RULE (%s): %d x %d at %d primes, min %d, k %d' % (fn, na, nb, np_, min(na, nb), k))
        if np_ == 3 and na + nb > k: moved += 1; ex = ex or (na, nb)
print('lines at 4 primes %d, at 3 %d (unchecked C-form lines %d); at 3 with min <= k < na + nb (moved by the switch): %d%s; against the rule: %d'
      % (n4, n3, nochk, moved, ' e.g. %d x %d' % ex if ex else '', bad))
sys.exit(1 if bad else 0)
