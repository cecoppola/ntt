#!/usr/bin/env python3
"""tools/test_unpack.py - Phase 15 IO (W2): a host-only test of tools/unpack_digits.

Makes random digit strings "2" + d digits (d a multiple of 18, as the run computes), writes them as packed parts exactly as
ecalc/mn_out.c does (ecalc/packed_fmt.h: the header, then the limbs most significant first; the parts split at the node
shares' limb boundaries), and checks that unpack_digits gives "2." + digits 1..d_out + "\\n" byte for byte (-o, stdout, --cmp),
that --cmp finds a changed byte and a length difference, and that a corrupted limb fails the residue check.

usage: tools/test_unpack.py [unpack_digits binary] [workdir]"""
import os, random, struct, subprocess, sys, tempfile

Q = [4611686018427388039, 4611686018427388073, 4611686018427388081, 4611686018427388091,
     4611686018427388093, 4611686018427388097, 4611686018427388157, 4611686018427388181]
B = 10 ** 18


def write_parts(base, digits, d, d_out, nparts):
    """digits: '2' + d chars.  Writes base (nparts == 1) or base.part%04d; returns the file names"""
    nl = (d + 1 + 17) // 18
    pad = nl * 18 - (d + 1)
    s = "0" * pad + digits
    limbs = [int(s[18 * (nl - 1 - i): 18 * (nl - i)]) for i in range(nl)]     # limb i: chars [18 (nl-1-i), 18 (nl-i))
    names = []
    for r in range(nparts):                                                   # comm_shard-like: rank r holds [lo, hi)
        lo, hi = nl * r // nparts, nl * (r + 1) // nparts
        if r == nparts - 1:
            hi = nl
        part = nparts - 1 - r
        p0 = (nl - hi) * 18
        k0 = p0 - pad if p0 > pad else 0
        k1 = (nl - lo) * 18 - pad
        dres = []                                                             # the digits [k0, k1) = the limbs [lo, hi) as a number
        for q in Q:
            v = 0
            for i in range(hi - 1, lo - 1, -1):
                v = (v * B + limbs[i]) % q
            dres.append(v)
        hdr = bytearray(4096)
        struct.pack_into("<8sIIIIIIIII", hdr, 0, b"ECPACK18", 1, 4096, 0x01020304, 8, 18, 1, part, nparts, 0)
        # (the magic and nine u32 end at 44; the u64 fields start at 48 after alignment)
        struct.pack_into("<10Q", hdr, 48, d, d_out, nl, pad, lo, hi, k0, k1, 1, 8)
        struct.pack_into("<8Q", hdr, 48 + 80, *Q)
        struct.pack_into("<8Q", hdr, 48 + 80 + 64, *dres)
        name = base if nparts == 1 else "%s.part%04d" % (base, part)
        with open(name, "wb") as f:
            f.write(hdr)
            f.write(b"".join(struct.pack("<Q", limbs[i]) for i in range(hi - 1, lo - 1, -1)))
        names.append(name)
    return sorted(names)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "unpack_digits")
    wd = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="unpack_test_")
    rnd = random.Random(15)
    fails = 0
    cases = [(18, 18, 1), (36, 20, 1), (1800, 1799, 1), (1800, 1783, 3), (100008, 100000, 4), (18 * 7000, 18 * 7000 - 5, 5),
             (18 * 300001, 18 * 300001 - 17, 2), (18 * 300001, 18 * 300001, 7)]
    for d, d_out, nparts in cases:
        digits = "2" + "".join(rnd.choice("0123456789") for _ in range(d))
        base = os.path.join(wd, "t_%d_%d_%d" % (d, d_out, nparts))
        names = write_parts(base, digits, d, d_out, nparts)
        want = ("2." + digits[1:d_out + 1] + "\n").encode()
        ref = base + ".ref"
        open(ref, "wb").write(want)
        out = base + ".out"
        r1 = subprocess.run([exe, "-q", "-c", "1", "-o", out] + names[::-1])          # parts in reverse: sorted by the headers
        ok1 = r1.returncode == 0 and open(out, "rb").read() == want
        r2 = subprocess.run([exe, "-q"] + names, stdout=subprocess.PIPE)
        ok2 = r2.returncode == 0 and r2.stdout == want
        r3 = subprocess.run([exe, "-q", "--cmp", ref] + names, stdout=subprocess.PIPE)
        ok3 = r3.returncode == 0
        bad = bytearray(want); pos = len(bad) // 2; bad[pos] = ord("0") + (bad[pos] - ord("0") + 1) % 10 if bad[pos] != ord(".") else bad[pos]
        open(ref + ".bad", "wb").write(bytes(bad))
        r4 = subprocess.run([exe, "-q", "--cmp", ref + ".bad"] + names, stdout=subprocess.PIPE)
        ok4 = (r4.returncode == 1) == (bytes(bad) != want)
        open(ref + ".long", "wb").write(want + b"7")
        r5 = subprocess.run([exe, "-q", "--cmp", ref + ".long"] + names, stdout=subprocess.PIPE)
        ok5 = r5.returncode == 1
        # a corrupted limb in the last part (not the header): the residue check must fail
        with open(names[-1], "r+b") as f:
            f.seek(4096); v = f.read(8); f.seek(4096); f.write(bytes([v[0] ^ 1]) + v[1:])
        r6 = subprocess.run([exe, "-q", "-n"] + names, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ok6 = r6.returncode == 1
        good = ok1 and ok2 and ok3 and ok4 and ok5 and ok6
        fails += not good
        print("d %8d d_out %8d parts %d: -o %s, stdout %s, --cmp %s, changed byte %s, longer ref %s, corrupt limb %s -> %s"
              % (d, d_out, nparts, ok1, ok2, ok3, ok4, ok5, ok6, "ok" if good else "FAIL"))
        for n in names + [ref, ref + ".bad", ref + ".long", out]:
            os.remove(n)
    print("test_unpack: %s" % ("PASS" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
