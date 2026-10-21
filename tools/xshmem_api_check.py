#!/usr/bin/env python3
"""tools/xshmem_api_check.py -- every function of the xshmem API table exists in both backends (brief B1).

The table is the `XAPI <name>` lines of the comment block in ecalc/xshmem.h.  Checked: the header declares each name (libfabric
prototype section), xshmem_cray.h defines each as a static inline, xshmem_ofi.c defines each (or xshmem_ofi_stub.c while the real file
has not landed: then reported as STUB).  Exit 1 on a missing name.  Read-only, host only.
"""
import os, re, sys
E = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ecalc")


def rd(n):
    p = os.path.join(E, n)
    return open(p, errors="replace").read() if os.path.exists(p) else None


def main():
    hdr = rd("xshmem.h")
    names = re.findall(r"^ \* XAPI (\w+)", hdr, re.M)
    if not names:
        print("no XAPI lines in xshmem.h"); return 1
    proto = hdr[hdr.index("#else\n/* ---- libfabric backend"):]
    cray = rd("xshmem_cray.h")
    ofi, which = rd("xshmem_ofi.c"), "xshmem_ofi.c"
    if ofi is None:
        ofi, which = rd("xshmem_ofi_stub.c"), "xshmem_ofi_stub.c (STUB)"
    bad = 0
    for n in names:
        miss = []
        if not re.search(r"\b%s\s*\(" % n, proto): miss.append("xshmem.h prototype")
        if not re.search(r"static inline [^;{]*\b%s\s*\(" % n, cray): miss.append("xshmem_cray.h")
        if ofi is None or not re.search(r"^[A-Za-z_][\w \*]*\b%s\s*\(" % n, ofi, re.M): miss.append(which)
        if miss:
            bad += 1; print("MISSING %s: %s" % (n, ", ".join(miss)))
    print("xshmem_api_check: %d functions, %d problems; ofi side = %s" % (len(names), bad, which))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
