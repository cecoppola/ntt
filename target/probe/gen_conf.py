#!/usr/bin/env python3
"""target/probe/gen_conf.py - turn probe facts into target/config/target.conf (contract: target/FORMATS.md section 8).

  gen_conf.py <login probe dir> [<node probe dir>] [-o target/config/target.conf] [--template FILE]

Reads facts.tsv (key TAB value TAB source) of the login part and optionally the node part (node facts win for the keys
only a node can know: memory, APUs, NICs).  Fills every key of the template that the facts determine, appends
"(probe DATE, fact KEY)" to its comment, leaves the rest at __FILL__ with a "why open" comment, writes the file,
prints a diff against the template, the open keys and advisories (NIC map, userfaultfd, MR cache monitor, SHMEM).
Read-only on the probe dirs.  Topology (host -> xname table) is not built here: ladder/locality.py nodes reads
<login dir>/raw/scontrol_nodes.txt (the saved `scontrol show node -o` of all nodes).
Exit 0 always when the inputs exist (open keys are reported, not an error); 2 = usage / unreadable input.
"""
import argparse, datetime, difflib, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BAD = {"", "missing", "none", "timeout", "skipped"}


def load(d):
    f = {}
    p = os.path.join(d, "facts.tsv")
    with open(p, errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2:
                f[parts[0]] = parts[1]          # the last value of a key wins
    return f


def good(v):
    return v is not None and v not in BAD and not v.startswith("missing")


def kv_list(s):
    """'a=1 b=2' -> dict"""
    return dict(m.groups() for m in re.finditer(r"(\S+?)=(\S+)", s or ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("login")
    ap.add_argument("node", nargs="?")
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "..", "config", "target.conf"))
    ap.add_argument("--template", default=os.path.join(HERE, "..", "config", "target.conf.template"))
    ap.add_argument("--want-nics-per-apu", type=int, default=2)
    a = ap.parse_args()
    try:
        L = load(a.login)
        N = load(a.node) if a.node else {}
        tpl = open(a.template).read().split("\n")
    except OSError as e:
        print("gen_conf: cannot read input: %s" % e, file=sys.stderr)
        return 2
    F = dict(L)
    F.update(N)                                  # node facts override (same key in both parts)
    today = datetime.date.today().isoformat()
    val, why, extra, adv = {}, {}, {}, []        # key -> value | key -> reason open | key -> raw replacement lines

    # ---- modules ----
    r, s, d = (F.get("mod.pick_" + x) for x in ("rocm", "sma", "dsmml"))
    for key, v, fk in (("TARGET_ROCM", r, "mod.pick_rocm"), ("TARGET_SMA_MODULE", s, "mod.pick_sma"),
                       ("TARGET_DSMML_MODULE", d, "mod.pick_dsmml")):
        if good(v):
            val[key] = (v, fk)
        else:
            why[key] = "no module matching the wanted version in `module avail` (%s=%s); check raw/module_avail.txt" % (fk, v)
    if good(r) and good(s) and good(d):
        val["MNRUN_MODULES"] = ('"%s %s %s"' % (d, s, r), "mod.pick_*")
    else:
        why["MNRUN_MODULES"] = "needs all three of rocm, sma and dsmml modules"
    if "mod.loaded_rocm" in F and F["mod.loaded_rocm"] != "missing":
        lr = F["mod.loaded_rocm"]
        val["MNRUN_UNLOAD"] = ('"%s"' % ("" if lr == "none" else lr), "mod.loaded_rocm")
    else:
        why["MNRUN_UNLOAD"] = "module list not collected (no module command in the probe shell)"

    # ---- GMP ----
    if good(F.get("lib.gmp_h")) and not F["lib.gmp_h"].startswith("/usr/"):
        val["GMP_HOME"] = (os.path.dirname(os.path.dirname(F["lib.gmp_h"])), "lib.gmp_h: a GMP outside the system paths")
    elif good(F.get("lib.gmp_h")):
        val["GMP_HOME"] = None                   # template: remove the key if the site provides gmp.h
        extra["GMP_HOME"] = "# GMP_HOME: not needed, the system provides %s (probe %s, lib.gmp_h)" % (F["lib.gmp_h"], today)
    else:
        why["GMP_HOME"] = "no gmp.h found by the probe (lib.gmp_h); build GMP from source or find the module (mod.avail_gmp=%s)" % F.get("mod.avail_gmp", "?")

    # ---- partition ----
    parts = {k[len("slurm.part."):]: kv_list(v) for k, v in F.items() if k.startswith("slurm.part.")}
    dflt = (F.get("slurm.default_partition") or "").strip()
    pick = dflt if good(dflt) else ""
    tn = lambda p: int(parts.get(p, {}).get("TotalNodes", "0") or 0)
    if parts:
        big = max(parts, key=tn)
        if not pick or (tn(pick) < 576 <= tn(big)):
            pick = big
    if pick:
        val["TARGET_PARTITION"] = (pick, "slurm.default_partition / slurm.part.%s TotalNodes=%s" % (pick, tn(pick)))
        if len(parts) > 1:
            adv.append("partitions: " + "; ".join("%s(nodes=%s MaxNodes=%s MaxTime=%s)" % (p, v.get("TotalNodes"), v.get("MaxNodes"), v.get("MaxTime"))
                                                   for p, v in sorted(parts.items())))
    else:
        why["TARGET_PARTITION"] = "no sinfo/scontrol partition facts (slurm.default_partition=%s)" % dflt

    # ---- node RAM -> ECALC_NODE_GB (template rule: MemAvailable on an idle compute node, less a 4 GB OS/service reserve) ----
    if good(N.get("mem.available_kb")):
        gb = int(int(re.match(r"\d+", N["mem.available_kb"]).group()) // 1048576) - 4
        val["ECALC_NODE_GB"] = (str(gb), "node mem.available_kb=%s: floor(GiB) - 4 reserve" % N["mem.available_kb"])
    else:
        why["ECALC_NODE_GB"] = "needs the node probe (mem.available_kb); run probe.sh --only node --jobid J"

    # ---- output directory: shared FS (lustre/gpfs/...) with the most free space ----
    shared = re.compile(r"lustre|gpfs|beegfs|daos|wekafs|panfs|nfs")
    cands = []
    for k, v in F.items():
        if k.startswith("fs.mount."):
            m = kv_list(v)
            if shared.search(m.get("type", "")):
                try:
                    cands.append((int(m["avail_kB"]), m["type"], m["mount"]))
                except (KeyError, ValueError):
                    pass
    cands.sort(reverse=True)
    # a candidate must be writable by this user: the probe's fs.writable_mounts lists the mount points it could write to; a mount that is not
    # in the list is used only through the user's own $SCRATCH below it.  Without that fact (older probe) nothing is filtered.
    wr = F.get("fs.writable_mounts")
    writable = set(wr.split()) if good(wr) or wr == "none" else None
    if wr == "none":
        writable = set()
    sc = F.get("fs.dir.SCRATCH", "")
    scd = sc.split()[0] if good(sc) else ""
    if writable is not None:
        cands = [c for c in cands if c[2] in writable or (scd and scd.startswith(c[2].rstrip("/") + "/"))]
    sysmnt = re.compile(r"/(apps|pe|opt|sw|usr|boot|var)(/|$)|^/home$")   # software / home mounts are no output candidates
    lustre = [c for c in cands if c[1] == "lustre"] or [c for c in cands if not sysmnt.search(c[2])] or cands
    if lustre:
        free, typ, mnt = lustre[0]
        base = mnt
        if scd and scd.startswith(mnt.rstrip("/") + "/"):
            base = scd                            # the user's own scratch dir lies on that file system
        val["ECALC_OUT_DIR"] = (base.rstrip("/") + "/ecalc_out", "fs.mount (%s, %s, %.1f TB free)%s" % (mnt, typ, free / 1e9, "; $SCRATCH" if base != mnt else "; CONFIRM a writable user dir"))
        val["MNACCEPT_TMP"] = (base.rstrip("/") + "/ecalc_tmp", "same file system as ECALC_OUT_DIR (not a RAM /tmp)")
        if len(lustre) > 1:
            adv.append("other shared file systems: " + "; ".join("%s %s %.1f TB free" % (c[2], c[1], c[0] / 1e9) for c in lustre[1:5]))
    else:
        hm = F.get("fs.dir.HOME", "")
        hd = hm.split()[0] if good(hm) else ""
        if hd:        # no shared file system the user can write to: the home file system (the user's own directory)
            val["ECALC_OUT_DIR"] = (hd.rstrip("/") + "/ecalc_out", "fs.dir.HOME: no writable shared file system in fs.mount.* / fs.writable_mounts; CONFIRM the quota")
            val["MNACCEPT_TMP"] = (hd.rstrip("/") + "/ecalc_tmp", "same file system as ECALC_OUT_DIR (not a RAM /tmp)")
            adv.append("ECALC_OUT_DIR is under the home directory: check its quota and free space (a full run writes ~0.417 B per digit)")
        else:
            for k in ("ECALC_OUT_DIR", "MNACCEPT_TMP"):
                why[k] = "no writable shared (lustre/gpfs/nfs) file system and no $HOME in the login facts (fs.mount.*, fs.writable_mounts, fs.dir.HOME)"

    val["MNRUN_EXCLUDE_HOSTS"] = ('""', "none flagged yet (the kit's nodechk stage fills it)")

    # ---- NIC map check: 2 NICs per APU, each on its APU's NUMA node ----
    npa, cxi, pci = F.get("net.nics_per_apu"), F.get("net.cxi_numa_map"), F.get("gpu.pci_numa_map")
    if good(npa):
        ok = abs(float(npa) - a.want_nics_per_apu) < 1e-9
        adv.append("NICs per APU: %s (cxi devices %s, APUs %s) -> %s" % (npa, F.get("net.cxi_count"), F.get("gpu.count"),
                   "as wanted, leave COMM_OFI_NICS unset" if ok else "NOT %d: set COMM_OFI_NICS" % a.want_nics_per_apu))
        if good(cxi) and good(pci):
            nic_numa = {t.split(":")[0]: t.split(":")[1] for t in cxi.split() if ":" in t}
            apu_numa = pci.split()
            mp = []
            for an in apu_numa:
                mp.append([n.replace("cxi", "") for n, nn in sorted(nic_numa.items()) if nn == an])
            bad_map = [m for m in mp if len(m) != a.want_nics_per_apu]
            adv.append("NIC<->NUMA: cxi %s | APU numa %s | per-APU NIC list %s" % (cxi, pci, ";".join(",".join(m) for m in mp)))
            if bad_map:
                if all(mp) and len(set(sum(mp, []))) == len(sum(mp, [])):
                    val["COMM_OFI_NICS"] = None
                    extra["COMM_OFI_NICS"] = "COMM_OFI_NICS=%s     # (probe %s) the NUMA map is not 2 per APU: one list per APU, ';' between APUs; CHECK before use" % (";".join(",".join(m) for m in mp), today)
                else:
                    adv.append("WARNING: NUMA map does not give each APU its own NICs (cxi NUMA ids are not unique per APU?); decide COMM_OFI_NICS by hand")
    else:
        adv.append("NIC check not possible: net.nics_per_apu=%s (needs the node probe)" % npa)

    # ---- advisories (nothing below is read by a script) ----
    adv.append("uffd: syscall %s, sysctl unprivileged_userfaultfd=%s; FI_MR_CACHE_MONITOR=userfaultfd is %s" %
               (F.get("uffd.syscall"), F.get("uffd.sysctl"), F.get("lib.fi_monitor.userfaultfd", "untested")))
    if F.get("lib.fi_monitor.userfaultfd") not in (None, "accepted", "missing", "none"):
        adv.append("WARNING: libfabric refuses the userfaultfd monitor here; the template value FI_MR_CACHE_MONITOR=userfaultfd needs the site / kit 'uffd' stage")
    adv.append("libfabric %s, cxi domains %s; ROCm %s; SMA prefix %s" % (F.get("lib.fi_version"), F.get("lib.fi_cxi_domain_count"),
               F.get("rocm.version_file"), F.get("lib.sma_prefix")))
    adv.append("APUs: %s x %s, VRAM bytes %s (ECALC_DEVICE_GB template 373.44 = 4 x 93.36 GB, measured on the target earlier; confirm with t_edge)" %
               (F.get("gpu.count"), F.get("gpu.names"), F.get("gpu.vram_bytes")))
    sh = {k[len("shmem.probe."):]: v for k, v in F.items() if k.startswith("shmem.probe.")}
    if sh:
        adv.append("SHMEM test: build %s; rc " % F.get("shmem.build") + " ".join("%s=%s" % (k[len("shmem.rc."):], v) for k, v in sorted(F.items()) if k.startswith("shmem.rc.")))
        n2, n4 = F.get("shmem.rc.nics2_1pe"), F.get("shmem.rc.nics4_1pe")
        if n2 == "0":
            adv.append("one PE accepts SHMEM_OFI_NUM_NICS=2 (%s for 4); one PE per APU can use both NICs" % ("accepted" if n4 == "0" else "refused"))
        elif n2:
            adv.append("one PE refused SHMEM_OFI_NUM_NICS=2 (rc %s): one NIC per PE" % n2)
        if "shmem_ptr_peers_nonnull=0/" in F.get("shmem.probe.twope", ""):
            adv.append("WARNING: shmem_ptr is NULL for a same-node peer: no on-node path")
    else:
        adv.append("SHMEM test results absent (node part not run, or --no-shmem)")

    # ---- write the conf from the template ----
    out, filled, opened = [], [], []
    for line in tpl:
        m = re.match(r"^([A-Z][A-Z0-9_]*)=(\S*)(\s*)(#.*)?$", line)
        if not m:
            out.append(line)
            continue
        k, v, sp, cm = m.group(1), m.group(2), m.group(3) or "  ", m.group(4) or ""
        if k in val and v == "__FILL__" and val[k] is None:     # key not needed: replace by a note
            out.append(extra[k])
            filled.append(k)
        elif k in val and v == "__FILL__":
            nv, src = val[k]
            out.append("%s=%s%s# (probe %s: %s) %s" % (k, nv, sp, today, src, cm.lstrip("# ")))
            filled.append(k)
        elif v == "__FILL__":
            out.append("%s=__FILL__%s# why open: %s; %s" % (k, sp, why.get(k, "the facts do not determine it"), cm.lstrip("# ")))
            opened.append(k)
        else:
            out.append(line)
        if k == "ECALC_NP" and "COMM_OFI_NICS" in extra:
            pass
    if "COMM_OFI_NICS" in extra:
        out.append(extra["COMM_OFI_NICS"])
    out += ["", "# ---- probe advisories (%s; generated by target/probe/gen_conf.py; nothing below is read by a script) ----" % today]
    out += ["# " + x for x in adv]
    out += ["# probe dirs: %s%s" % (os.path.abspath(a.login), " + " + os.path.abspath(a.node) if a.node else "")]
    outp = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(outp), exist_ok=True)
    with open(outp, "w") as fh:
        fh.write("\n".join(out))
    print("wrote %s" % outp)
    print("=== diff against the template (changed lines) ===")
    for l in difflib.unified_diff(tpl, out[:len(tpl) + 2], "template", "target.conf", n=0, lineterm=""):
        if l.startswith(("+", "-")) and not l.startswith(("+++", "---")):
            print(l[:200])
    print("=== filled (%d): %s" % (len(filled), " ".join(filled)))
    print("=== open keys (%d): %s" % (len(opened), " ".join(opened)))
    for k in opened:
        print("  %s: %s" % (k, why.get(k, "the facts do not determine it")))
    print("=== advisories")
    for x in adv:
        print("  " + x[:300])
    print("topology: ladder/locality.py nodes --from %s" % os.path.join(os.path.abspath(a.login), "raw", "scontrol_nodes.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
