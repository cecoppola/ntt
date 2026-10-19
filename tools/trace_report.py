#!/usr/bin/env python3
"""tools/trace_report.py -- the report of a COMM_TRACE=1 run (ecalc/comm_trace.h; switches in ecalc/README.md).

  python3 tools/trace_report.py [options] RUN [RUN ...]       RUN = DIR or LABEL=DIR; DIR holds the run's trace_pe<PE>.bin files
  python3 tools/trace_report.py --selftest                    a synthetic 3-node trace through the whole analysis (exit 1 on a failure)

Reads the per-process files (node-processes and proxies) with numpy memory maps in chunks, puts every stamp on node 0's clock (the ping-pong offsets of the
header, linear in time between the init and the finalize estimate), spills the node-process records per communicator to a temporary directory
(--tmp, default $TMPDIR) and analyses one communicator at a time, so the memory is one communicator's records (the largest: about a third of the trace)
plus compact per-message tables (capped at --max-msgs, uniformly thinned beyond).  Output: a compact text report (--out FILE, default stdout):
  1 header     files, clock offsets, drift and round-trip bounds, sampling, drops
  2 volume     exchanges, messages, bytes per exchange kind and path
  3 stages     latency p50 / p90 / p99 / max of every stage of an exchange, of a round and of a message (sender wait for the offset word, put post, wire /
               proxy stages, signal one way, end to end); one block per RUN and a side-by-side comparison when there are several
  4 links      per source node -> destination node and per peer distance (shift order): messages, end to end, signal one way, rate
  5 last       who is last: per receiver (the sender whose signal was seen last, by how much), per exchange (the sender node that issued last, the node that
               published its offset word last), which peer ended the waits, with the lag against the median
  6 slowest    the slowest exchanges: per rank stage breakdown, the slowest messages, and the raw event timeline of the slowest
  6b skew      the slowest exchanges by arrival skew (last begin - first begin) with the late node; sections 6 and 7 rank by xt = last done - last begin (after the last arrival)
  7 time       whether slow exchanges / messages cluster in time (a node stalling) or are spread: per-bin counts, Fano factor, persistence, worst bins
  8 stalls     (COMM_TRACE_HB) the heartbeat gaps of every node (node-process and proxy threads), the OS counters that moved at each gap (vmstat compaction / THP / faults / swap,
               pressure, the thread's context switches) and the late exchanges of that node at that time; the skew-ranked exchanges against the heartbeat of their late node
  8a           (COMM_TRACE_HB_MEM) per node, the seconds with pgmigrate_fail > 20000/s with fragmentation score, MemFree, Dirty+Writeback, kcompactd ticks and KFD evicted_ms around them
  8b           (COMM_TRACE_PH, _THR, _WD_MS) the progress-watchdog firings and the slowest exchanges' late nodes: the phase tag of every thread of the node-process at the onset (ecalc/comm_phase.h),
               the thread snapshot (state, wchan, run_delay, CPU) and the heartbeat state (alive / gaps)
A communicator is identified by (id, the node of each rank): ids repeat across disjoint node groups (layeq pairs), shown as id@nodes.
Times of different nodes are only as good as the clock estimate (bound = half the best round trip, printed); 'neg%' counts cross-node one-way latencies
below zero (a measure of the clock error against the real latency)."""
import sys, os, json, struct, glob, math, tempfile, shutil, argparse, collections, bisect

try:
    import numpy as np
except ImportError:
    sys.exit("trace_report.py: needs numpy (module load python / the cluster's python3)")

REC = np.dtype([('t', '<u8'), ('bytes', '<u8'), ('comm', '<u4'), ('seq', '<u4'), ('xseq', '<u4'), ('aux', '<u4'), ('round', '<u2'), ('ev', 'u1'), ('kp', 'u1'), ('me', '<i2'), ('peer', '<i2')])
REC2 = np.dtype([('t', '<i8')] + [(n, REC[n]) for n in REC.names[1:]])         # the spilled record: t = ns on node 0's clock, relative to node 0's init
assert REC.itemsize == 40 and REC2.itemsize == 40
KIND = ["other", "redist", "result", "spill", "addsh", "layeq", "layv"]
PATH = ["equal", "v", "rounds"]
NAN = float("nan")
PXNONE = 0xFFFFFFFF


# ---------------------------------------------------------------- files, clocks
class Run:
    pass


def read_head(p):
    with open(p, "rb") as f:
        hd = f.read(64)
        magic, rb, jl, jo, ro, nrec, dropped = struct.unpack("<8sIIQQQQ", hd[:48])
        if magic != b"ECTRACE1" or rb != 40:
            raise SystemExit("%s: not a trace file (magic %r, record %d bytes)" % (p, magic, rb))
        f.seek(jo); meta = json.loads(f.read(jl).decode())
    return dict(path=p, meta=meta, nrec=nrec, ro=ro, dropped=dropped)


def load_run(d, label):
    r = Run(); r.label = label; r.dir = d
    r.files = [read_head(p) for p in sorted(glob.glob(os.path.join(d, "trace_pe*.bin")))]
    if not r.files:
        raise SystemExit("%s: no trace_pe*.bin files" % d)
    r.nodeproc = [h for h in r.files if not h["meta"]["proxy"]]
    r.proxies = [h for h in r.files if h["meta"]["proxy"]]
    r.nodes = max(h["meta"]["nodes"] for h in r.files)
    r.meta0 = next((h["meta"] for h in r.nodeproc if h["meta"]["node"] == 0), r.nodeproc[0]["meta"])
    r.T0 = r.meta0["m0"]
    r.ev = {n: i + 1 for i, n in enumerate(r.files[0]["meta"]["events"])}
    r.clock = {}                                   # node -> (off0, x0, off1, x1, rtt, rtt_med)
    passes = r.meta0.get("clock") or []
    p0 = {c["node"]: c for c in passes[0]} if len(passes) > 0 else {}
    p1 = {c["node"]: c for c in passes[1]} if len(passes) > 1 else {}
    for j in range(1, r.nodes):
        if j in p0:
            a = p0[j]; b = p1.get(j)
            r.clock[j] = (a["off_m"], a["t0"], b["off_m"] if b else None, b["t0"] if b else None, min(a["rtt"], b["rtt"]) if b else a["rtt"], a["rtt_med"])
    # a communicator id is only unique per group of nodes (the layeq pairs of nodes 0-1, 2-3, 4-5 all use id 8): the analysis unit is the group (id, node of each rank)
    r.comms = {}                                   # group index -> dict(id, n, node[], apu, lab)
    groups = {}
    for h in r.nodeproc:
        lut = {}
        for c in h["meta"]["comms"]:
            key = (c["id"], tuple(c["node"]))
            if key not in groups:
                groups[key] = len(groups); r.comms[groups[key]] = dict(id=c["id"], n=c["n"], node=c["node"], apu=c["apu"])
            lut[c["id"]] = groups[key]
        h["lut"] = lut
    idc = collections.Counter(c["id"] for c in r.comms.values())
    for g, c in r.comms.items():
        c["lab"] = str(c["id"]) if idc[c["id"]] == 1 else "%d@%s" % (c["id"], "-".join(str(x) for x in sorted(set(c["node"]))))
    return r


def to_clock0(run, t_u8, node):
    t = t_u8.astype(np.int64)
    if node == 0 or node not in run.clock:
        return t - np.int64(run.T0)
    o0, x0, o1, x1, _, _ = run.clock[node]
    if o1 is None or x1 == x0:
        return t - np.int64(o0) - np.int64(run.T0)
    rate = (o1 - o0) / float(x1 - x0)
    off = o0 + (t - np.int64(o0 + x0)).astype(np.float64) * rate
    return t - np.rint(off).astype(np.int64) - np.int64(run.T0)


def chunks(h, n=2000000):
    if h["nrec"] == 0:
        return
    mm = np.memmap(h["path"], dtype=REC, mode="r", offset=h["ro"], shape=(h["nrec"],))
    for i in range(0, h["nrec"], n):
        yield mm[i:i + n]


# ---------------------------------------------------------------- proxies: dense per-command tables
def load_proxies(run):
    P = {}
    E = run.ev
    for h in run.proxies:
        node, apu = h["meta"]["node"], h["meta"]["apu"]
        kmax = -1
        for c in chunks(h):
            kmax = max(kmax, int(c["aux"].max()))
        if kmax < 0:
            continue
        tab = {k: np.full(kmax + 1, NAN) for k in ("pick", "cfirst", "clast", "sig", "done", "qbeg", "qend", "batch")}
        tab["nch"] = np.zeros(kmax + 1, np.int32); tab["bytes"] = np.zeros(kmax + 1, np.uint64)
        for c in chunks(h):
            t = to_clock0(run, c["t"], node).astype(np.float64); k = c["aux"].astype(np.int64); ev = c["ev"]
            for name, key in (("P_PICKUP", "pick"), ("P_SIGNAL", "sig"), ("P_DONE", "done"), ("P_QUIET_BEG", "qbeg"), ("P_QUIET_END", "qend"), ("P_BATCH", "batch")):
                m = ev == E[name]
                if m.any():
                    np.fmin.at(tab[key], k[m], t[m])
            m = ev == E["P_PICKUP"]
            if m.any():
                tab["bytes"][k[m]] = c["bytes"][m]
            m = ev == E["P_CHUNK"]
            if m.any():
                np.fmin.at(tab["cfirst"], k[m], t[m]); np.fmax.at(tab["clast"], k[m], t[m]); np.add.at(tab["nch"], k[m], 1)
        P[(node, apu)] = tab
    return P


# ---------------------------------------------------------------- spill the node-process records per communicator
def spill(run, tmp):
    fh = {}; counts = collections.Counter(); evh = run.ev.get("H_GAP", 256)
    for h in run.nodeproc:
        node = h["meta"]["node"]; lut = np.full(4097, 0xFFFFFFFF, np.uint32)
        for i, g in h["lut"].items():
            if i < 4096:
                lut[i] = g
        for c in chunks(h):
            c = c[c["ev"] < evh]                    # the heartbeat records (H_*) are not exchange records
            if len(c) == 0:
                continue
            out = np.empty(len(c), REC2)
            for nme in REC.names[1:]:
                out[nme] = c[nme]
            out["t"] = to_clock0(run, c["t"], node); out["comm"] = lut[np.minimum(c["comm"], 4096)]
            order = np.argsort(out["comm"], kind="stable"); out = out[order]; cm = out["comm"]
            brk = np.flatnonzero(np.r_[True, cm[1:] != cm[:-1]]); ends = np.r_[brk[1:], len(out)]
            for s, e in zip(brk, ends):
                cid = int(cm[s])
                if cid not in fh:
                    fh[cid] = open(os.path.join(tmp, "c%d.bin" % cid), "ab")
                out[s:e].tofile(fh[cid]); counts[cid] += int(e - s)
    for f in fh.values():
        f.close()
    return counts


# ---------------------------------------------------------------- helpers
def grp(keys):
    order = np.argsort(keys, kind="stable"); ks = keys[order]
    starts = np.flatnonzero(np.r_[True, ks[1:] != ks[:-1]]) if len(ks) else np.zeros(0, np.int64)
    return order, starts, ks[starts] if len(ks) else ks


def last_per_key(keys, vals):
    """sorted unique keys and the value of the LAST (emission order) record of each key"""
    if len(keys) == 0:
        return keys, vals
    order = np.argsort(keys, kind="stable"); ks = keys[order]; vs = vals[order]
    last = np.r_[ks[1:] != ks[:-1], True]
    return ks[last], vs[last]


def lookup(base, uk, uv, dtype=np.float64, fill=NAN):
    if len(uk) == 0:
        return np.full(len(base), fill, dtype)
    pos = np.searchsorted(uk, base); pos[pos >= len(uk)] = len(uk) - 1
    ok = uk[pos] == base
    return np.where(ok, uv[pos].astype(dtype), dtype(fill) if dtype != np.float64 else fill)


def mkey(src, dst, seq, rnd):
    return (src.astype(np.uint64) << np.uint64(56)) | (dst.astype(np.uint64) << np.uint64(48)) | (seq.astype(np.uint64) << np.uint64(16)) | rnd.astype(np.uint64)


def pct(v, unit_div=1.0):
    v = np.asarray(v, np.float64); v = v[np.isfinite(v)] / unit_div
    if len(v) == 0:
        return None
    q = np.percentile(v, [50, 90, 99])
    return dict(n=len(v), p50=q[0], p90=q[1], p99=q[2], max=v.max(), mean=v.mean(), neg=100.0 * float((v < 0).mean()))


# ---------------------------------------------------------------- exchange / round / message tables of one communicator
def ex_table(R, E):
    ev = R["ev"]; out = {}
    b = R[ev == E["E_BEGIN"]]
    if len(b) == 0:
        return None
    bk = (b["me"].astype(np.uint64) << np.uint64(32)) | b["xseq"].astype(np.uint64)
    bk, bi = last_per_key(bk, np.arange(len(b)))
    b = b[bi]; out["key"] = bk; out["me"] = b["me"].astype(np.int64); out["xseq"] = b["xseq"].astype(np.int64); out["t_begin"] = b["t"].astype(np.float64)
    out["bytes"] = b["bytes"].astype(np.float64); out["kind"] = (b["kp"] & 15).astype(np.int64)
    for name, col in (("E_COPY_ISS", "t_copy_iss"), ("E_COPY_DONE", "t_copy"), ("E_PUBLISHED", "t_pub"), ("E_POSTED", "t_posted"), ("E_WAIT", "t_wait"), ("E_JOINED", "t_joined"),
                      ("E_SIGS_SEEN", "t_sigs"), ("E_QUIET_END", "t_quiet"), ("E_RCOPY_ISS", "t_rciss"), ("E_RCOPY_DONE", "t_rcopy"), ("E_DONE", "t_done")):
        x = R[ev == E[name]]; k = (x["me"].astype(np.uint64) << np.uint64(32)) | x["xseq"].astype(np.uint64)
        uk, uv = last_per_key(k, x["t"].astype(np.float64)); out[col] = lookup(bk, uk, uv)
        if name == "E_DONE":
            uk, uv = last_per_key(k, (x["kp"] >> 4).astype(np.int64)); out["path"] = lookup(bk, uk, uv, np.int64, 0)
        if name == "E_POSTED":
            uk, uv = last_per_key(k, x["aux"].astype(np.int64)); out["K"] = lookup(bk, uk, uv, np.int64, 1)
        if name in ("E_COPY_DONE", "E_RCOPY_DONE"):                    # bytes = the GPU time of the copy between two hipEvents (0: not recorded)
            uk, uv = last_per_key(k, x["bytes"].astype(np.float64)); g = lookup(bk, uk, uv); g[g <= 0] = NAN; out["copy_gpu" if name == "E_COPY_DONE" else "rcopy_gpu"] = g
    return out


def rd_table(R, E):
    ev = R["ev"]; b = R[ev == E["R_BEGIN"]]
    if len(b) == 0:
        return None
    key = lambda x: (x["me"].astype(np.uint64) << np.uint64(48)) | (x["xseq"].astype(np.uint64) << np.uint64(16)) | x["round"].astype(np.uint64)
    bk, bt = last_per_key(key(b), b["t"].astype(np.float64)); out = {"t_begin": bt}
    for name, col in (("R_PUBLISHED", "pub"), ("R_SCOPY_ISS", "sciss"), ("R_SCOPY_DONE", "scopy"), ("R_SENDS_DONE", "sends"), ("R_SIGS_DONE", "sigs"), ("R_QUIET_END", "quiet"), ("R_RCOPY_ISS", "rciss"), ("R_END", "end")):
        x = R[ev == E[name]]; uk, uv = last_per_key(key(x), x["t"].astype(np.float64)); out[col] = lookup(bk, uk, uv)
    return out


def msg_table(R, E, nodes_of_rank, apu, P):
    ev = R["ev"]; g = lambda n: R[ev == E[n]]
    pi = g("M_PUT_ISSUE")
    if len(pi) == 0:
        return None
    sk = lambda x: mkey(x["me"], x["peer"], x["seq"], x["round"])          # sender stamps: me = data sender
    rk = lambda x: mkey(x["peer"], x["me"], x["seq"], x["round"])          # receiver stamps: me = data receiver
    bk, bi = last_per_key(sk(pi), np.arange(len(pi))); pi = pi[bi]
    m = {"src": pi["me"].astype(np.int64), "dst": pi["peer"].astype(np.int64), "seq": pi["seq"].astype(np.int64), "rnd": pi["round"].astype(np.int64), "xseq": pi["xseq"].astype(np.int64),
         "bytes": pi["bytes"].astype(np.float64), "kind": (pi["kp"] & 15).astype(np.int64), "path": (pi["kp"] >> 4).astype(np.int64), "t_put": pi["t"].astype(np.float64)}
    for name, col, keyf in (("M_PUT_POSTED", "t_posted", sk), ("M_PUT_DONE", "t_done", sk), ("M_SIG_ISSUE", "t_sigi", sk), ("M_ROFF_WAIT", "t_roffw", sk), ("M_ROFF_SEEN", "t_roffs", sk),
                            ("M_ROFF_ISSUE", "t_roffi", rk), ("M_SIG_WAIT", "t_sigw", rk), ("M_SIG_SEEN", "t_sigs", rk)):
        x = g(name); uk, uv = last_per_key(keyf(x), x["t"].astype(np.float64)); m[col] = lookup(bk, uk, uv)
    x = g("M_PUT_POSTED"); uk, uv = last_per_key(sk(x), x["aux"].astype(np.int64)); m["pxk"] = lookup(bk, uk, uv, np.int64, PXNONE)
    nr = np.array(nodes_of_rank, np.int64); m["nsrc"] = nr[m["src"]]; m["ndst"] = nr[m["dst"]]; m["dist"] = (m["dst"] - m["src"]) % len(nr)
    n = len(bk)
    for c in ("pick", "cfirst", "clast", "psig", "pdone"):
        m[c] = np.full(n, NAN)
    for node in np.unique(m["nsrc"]):
        t = P.get((int(node), apu))
        if t is None:
            continue
        sel = np.flatnonzero((m["nsrc"] == node) & (m["pxk"] != PXNONE) & (m["pxk"] < len(t["pick"])))
        kk = m["pxk"][sel]
        for col, key in (("pick", "pick"), ("cfirst", "cfirst"), ("clast", "clast"), ("psig", "sig"), ("pdone", "done")):
            m[col][sel] = t[key][kk]
    # derived stages (ns)
    d = {}
    d["roff_wait"] = m["t_roffs"] - m["t_roffw"]                 # sender blocked on the receiver's offset word
    d["roff_oneway"] = m["t_roffs"] - m["t_roffi"]               # receiver's word issued -> sender saw it (cross-node; includes the sender's lateness)
    d["put_post"] = m["t_posted"] - m["t_put"]                   # put / ring-post call
    d["wire"] = m["t_done"] - m["t_posted"]                      # libfabric / PX1: put posted -> delivery complete (observed by the sender)
    d["px_ring"] = m["pick"] - m["t_posted"]                     # proxy: ring post -> the proxy takes the command
    d["px_chunks"] = m["clast"] - m["cfirst"]                    # proxy: first -> last chunk issued
    d["px_busy"] = m["pdone"] - m["pick"]                        # proxy: pickup -> command's quiet done
    sig_issue = np.where(np.isfinite(m["t_sigi"]), m["t_sigi"], m["psig"])
    d["sig_oneway"] = m["t_sigs"] - sig_issue                    # signal issued -> receiver saw it (cross-node)
    d["sig_wait"] = m["t_sigs"] - m["t_sigw"]                    # receiver blocked on the signal
    d["e2e"] = m["t_sigs"] - m["t_put"]                          # put issued -> signal seen (cross-node)
    d["rate_gbs"] = m["bytes"] / np.where(d["e2e"] > 0, d["e2e"], NAN)   # bytes per ns = GB/s
    m["sig_issue"] = sig_issue
    return m, d


# ---------------------------------------------------------------- accumulation across communicators
MSG_STAGES = ["roff_wait", "roff_oneway", "put_post", "wire", "px_ring", "px_chunks", "px_busy", "sig_oneway", "sig_wait", "e2e"]
EX_STAGES = [("copy_sync", lambda e: e["t_copy"] - e["t_copy_iss"]), ("copy_gpu", lambda e: e["copy_gpu"]), ("rcopy_gpu", lambda e: e["rcopy_gpu"]), ("post", lambda e: e["t_posted"] - e["t_begin"]), ("copy", lambda e: e["t_copy"] - e["t_begin"]), ("publish", lambda e: e["t_pub"] - e["t_copy"]),
             ("call", lambda e: e["t_wait"] - e["t_posted"]), ("join", lambda e: e["t_joined"] - e["t_wait"]), ("sigs", lambda e: e["t_sigs"] - e["t_joined"]),
             ("quiet", lambda e: e["t_quiet"] - e["t_sigs"]), ("rcopy", lambda e: e["t_rcopy"] - e["t_rciss"]), ("wait", lambda e: e["t_done"] - e["t_wait"]), ("total", lambda e: e["t_done"] - e["t_begin"])]
RD_STAGES = [("publish", lambda r: r["pub"] - r["t_begin"]), ("scopy", lambda r: r["scopy"] - r["pub"]), ("sends", lambda r: r["sends"] - np.where(np.isfinite(r["scopy"]), r["scopy"], r["pub"])),
             ("sigs", lambda r: r["sigs"] - r["sends"]), ("quiet", lambda r: r["quiet"] - r["sigs"]), ("rcopy", lambda r: r["end"] - r["quiet"]), ("round", lambda r: r["end"] - r["t_begin"])]


class Acc:
    def __init__(self, cap):
        self.cap = cap; self.stage = collections.defaultdict(list); self.msgs = []; self.nmsg = 0; self.exs = []; self.nex = 0
        self.last = collections.defaultdict(list); self.top = []; self.vol = collections.defaultdict(lambda: [0, 0, 0.0, 0.0, 1e30, 0.0]); self.keep = 1.0
        self.ncomm = 0; self.topskew = []


def thin(arr_dict, frac, rng):
    n = len(next(iter(arr_dict.values())))
    if frac >= 1.0:
        return arr_dict
    sel = np.flatnonzero(rng.random(n) < frac)
    return {k: v[sel] for k, v in arr_dict.items()}


def last_analysis(m, d, nodes, acc):
    """who is last: by receiver, by exchange, which peer ended the waits"""
    n = len(m["src"]); ok = np.isfinite(m["t_sigs"])
    # (a) per receiver, per (xseq, round): the sender seen last
    sel = np.flatnonzero(ok)
    if len(sel):
        key = (m["xseq"][sel].astype(np.uint64) << np.uint64(32)) | (m["rnd"][sel].astype(np.uint64) << np.uint64(16)) | m["dst"][sel].astype(np.uint64)
        order = np.lexsort((m["t_sigs"][sel], key)); ks = key[order]; ts = m["t_sigs"][sel][order]; src = m["nsrc"][sel][order]; dist = m["dist"][sel][order]
        ends = np.flatnonzero(np.r_[ks[1:] != ks[:-1], True]); two = ends[(ends > 0)]; two = two[ks[two] == ks[two - 1]]
        for nd, ds, gap in zip(src[two], dist[two], ts[two] - ts[two - 1]):
            acc.last["recv_last"].append((int(nd), int(ds), float(gap)))
    # (b) per (xseq, round): sender node that issued its last signal last; lag against the median of the senders' last issue
    sel = np.flatnonzero(np.isfinite(m["sig_issue"]))
    if len(sel):
        gk = (m["xseq"][sel].astype(np.uint64) << np.uint64(16)) | m["rnd"][sel].astype(np.uint64)
        sk = gk * np.uint64(256) + m["src"][sel].astype(np.uint64)
        order, st, uk = grp(sk); lastiss = np.fmax.reduceat(m["sig_issue"][sel][order], st); g2 = uk // np.uint64(256); s2 = (uk % np.uint64(256)).astype(np.int64)
        node_of_src = {}
        for s, nd in zip(m["src"], m["nsrc"]):
            node_of_src[int(s)] = int(nd)
        o2 = np.lexsort((lastiss, g2)); g2o = g2[o2]; ti = lastiss[o2]; so = s2[o2]
        brk = np.flatnonzero(np.r_[True, g2o[1:] != g2o[:-1]]); en = np.r_[brk[1:], len(g2o)]
        for a, b in zip(brk, en):
            if b - a >= 2:
                acc.last["ex_issue"].append((node_of_src[int(so[b - 1])], float(ti[b - 1] - np.median(ti[a:b])), float(ti[b - 1] - ti[b - 2])))
    # (c) per (xseq, round): the receiver whose offset word was published last
    sel = np.flatnonzero(np.isfinite(m["t_roffi"]))
    if len(sel):
        gk = (m["xseq"][sel].astype(np.uint64) << np.uint64(16)) | m["rnd"][sel].astype(np.uint64); rk = gk * np.uint64(256) + m["dst"][sel].astype(np.uint64)
        order, st, uk = grp(rk); lastpub = np.fmax.reduceat(m["t_roffi"][sel][order], st); g2 = uk // np.uint64(256); r2 = (uk % np.uint64(256)).astype(np.int64)
        node_of_dst = {}
        for s, nd in zip(m["dst"], m["ndst"]):
            node_of_dst[int(s)] = int(nd)
        o2 = np.lexsort((lastpub, g2)); g2o = g2[o2]; ti = lastpub[o2]; ro = r2[o2]
        brk = np.flatnonzero(np.r_[True, g2o[1:] != g2o[:-1]]); en = np.r_[brk[1:], len(g2o)]
        for a, b in zip(brk, en):
            if b - a >= 2:
                acc.last["ex_pub"].append((node_of_dst[int(ro[b - 1])], float(ti[b - 1] - np.median(ti[a:b])), float(ti[b - 1] - ti[b - 2])))
    # (d) which peer ended the waits: per sender, per (xseq, round), the receiver whose offset word was awaited longest; per receiver the sender whose signal was awaited longest
    for col, grpkey, who, whonode in (("roff_wait", "src", "dst", "ndst"), ("sig_wait", "dst", "src", "nsrc")):
        sel = np.flatnonzero(np.isfinite(d[col]))
        if not len(sel):
            continue
        key = (m["xseq"][sel].astype(np.uint64) << np.uint64(32)) | (m["rnd"][sel].astype(np.uint64) << np.uint64(16)) | m[grpkey][sel].astype(np.uint64)
        order = np.lexsort((d[col][sel], key)); ks = key[order]; ends = np.flatnonzero(np.r_[ks[1:] != ks[:-1], True])
        w = d[col][sel][order][ends]; nd = m[whonode][sel][order][ends]; ds = m["dist"][sel][order][ends]
        for a, b, c in zip(nd, ds, w):
            acc.last["end_" + col].append((int(a), int(b), float(c)))


def add_comm(R, run, cid, cinfo, P, acc, rng, top_k):
    E = run.ev
    ex = ex_table(R, E); rd = rd_table(R, E); mt = msg_table(R, E, cinfo["node"], cinfo["apu"], P)
    if ex is not None:
        for name, f in EX_STAGES:
            for kd in np.unique(ex["kind"]):
                v = f(ex)[ex["kind"] == kd]; acc.stage[("ex", name, int(kd))].append(v[np.isfinite(v)].astype(np.float32))
        # volume + per-exchange summary (across ranks)
        for kd in np.unique(ex["kind"]):
            for pa in np.unique(ex["path"]):
                s = (ex["kind"] == kd) & (ex["path"] == pa)
                if s.any():
                    a = acc.vol[(int(kd), int(pa))]; a[0] += int(s.sum()); a[2] += float(ex["bytes"][s].sum())
        order, st, uk = grp(ex["xseq"]); n = len(uk)
        tb = np.fmin.reduceat(ex["t_begin"][order], st); td = np.fmax.reduceat(ex["t_done"][order], st); tl = np.fmax.reduceat(ex["t_begin"][order], st); skew = tl - tb
        dur_rank = np.fmax.reduceat((ex["t_done"] - ex["t_begin"])[order], st)
        o2 = np.lexsort((ex["t_begin"], ex["xseq"])); xs2 = ex["xseq"][o2]; en = np.flatnonzero(np.r_[xs2[1:] != xs2[:-1], True]); st2 = np.r_[0, en[:-1] + 1]
        nodes_arr = np.array(cinfo["node"], np.int64); late_node = nodes_arr[ex["me"][o2][en]]; first_node = nodes_arr[ex["me"][o2][st2]]
        # skew = last arrival (E_BEGIN) - first arrival; xt = the exchange time after the last arrival = last done - last begin (what the transport and the copies cost)
        e = dict(comm=np.full(n, cid), xseq=uk, kind=ex["kind"][order][st], path=ex["path"][order][st], bytes=np.add.reduceat(ex["bytes"][order], st), t_start=tb, t_end=td, dur=td - tb,
                 durmax=dur_rank, skew=skew, K=ex["K"][order][st], t_last=tl, xt=td - tl, late_node=late_node, first_node=first_node)
        acc.exs.append(e); acc.nex += n
        cand = np.argsort(-np.nan_to_num(e["xt"], nan=-1))[:top_k]
        for i in cand:
            sub = R[R["xseq"] == e["xseq"][i]]
            acc.top.append((float(e["xt"][i]), cid, int(e["xseq"][i]), sub.copy()))
        acc.top.sort(key=lambda x: -x[0]); del acc.top[top_k:]
        for i in np.argsort(-np.nan_to_num(skew, nan=-1))[:15]:
            acc.topskew.append(dict(skew=float(skew[i]), cid=cid, xseq=int(uk[i]), kind=int(e["kind"][i]), late=int(late_node[i]), first=int(first_node[i]), t_start=float(tb[i]), t_last=float(tl[i]), xt=float(e["xt"][i]), bytes=float(e["bytes"][i])))
        acc.topskew.sort(key=lambda x: -x["skew"]); del acc.topskew[15:]
    if rd is not None:
        for name, f in RD_STAGES:
            v = f(rd); acc.stage[("rd", name, -1)].append(v[np.isfinite(v)].astype(np.float32))
    if mt is not None:
        m, d = mt
        n = len(m["src"])
        for kd in np.unique(m["kind"]):
            for pa in np.unique(m["path"]):
                s = (m["kind"] == kd) & (m["path"] == pa)
                if s.any():
                    a = acc.vol[(int(kd), int(pa))]; a[1] += int(s.sum()); a[3] += float(m["bytes"][s].sum()); a[4] = min(a[4], float(m["bytes"][s].min())); a[5] = max(a[5], float(m["bytes"][s].max()))
        for name in MSG_STAGES:
            for kd in np.unique(m["kind"]):
                v = d[name][m["kind"] == kd]; acc.stage[("msg", name, int(kd))].append(v[np.isfinite(v)].astype(np.float32))
        last_analysis(m, d, run.nodes, acc)
        keep = dict(nsrc=m["nsrc"].astype(np.int16), ndst=m["ndst"].astype(np.int16), dist=m["dist"].astype(np.int16), kind=m["kind"].astype(np.int8), path=m["path"].astype(np.int8), bytes=m["bytes"].astype(np.float32),
                    t_put=m["t_put"], e2e=d["e2e"].astype(np.float32), sig=d["sig_oneway"].astype(np.float32), rw=d["roff_wait"].astype(np.float32), wire=np.where(np.isfinite(d["wire"]), d["wire"], d["px_busy"]).astype(np.float32),
                    comm=np.full(n, cid, np.int32), xseq=m["xseq"].astype(np.int32))
        acc.nmsg += n
        acc.msgs.append(keep)
        tot = sum(len(x["e2e"]) for x in acc.msgs)
        if tot > acc.cap:                                  # thin everything uniformly (documented in the header of the report)
            f = acc.cap / float(tot) * 0.9; acc.keep *= f
            acc.msgs = [thin(x, f, rng) for x in acc.msgs]


# ---------------------------------------------------------------- the report
def fmtp(p, w=9):
    if p is None:
        return "%-*s" % (6 * w, "-")
    return "%7d %*.*f %*.*f %*.*f %*.*f %*.*f" % (p["n"], w, 3, p["p50"], w, 3, p["p90"], w, 3, p["p99"], w, 3, p["max"], w, 3, p["mean"])


def stage_summary(acc):
    """{(layer, stage, kind): pct dict in the layer's unit} (ex, rd in ms; msg in us); kind -1 = all"""
    out = {}
    byk = collections.defaultdict(list)
    for (layer, name, kd), arrs in acc.stage.items():
        if arrs:
            byk[(layer, name)].append((kd, np.concatenate(arrs)))
    for (layer, name), lst in byk.items():
        div = 1e3 if layer == "msg" else 1e6
        if name == "rate_gbs":
            div = 1.0
        for kd, v in lst:
            out[(layer, name, kd)] = pct(v, div)
        if len(lst) > 1:
            out[(layer, name, -1)] = pct(np.concatenate([v for _, v in lst]), div)
    return out


def section_header(run, L):
    L.append("== RUN %s  (%s) ==" % (run.label, run.dir))
    m = run.meta0
    L.append("1 header: %d node-process files, %d proxy files, %d nodes; ppa %s pxsend %s order %s shift %s round_mb %s; sampling 1/%s; records %d, dropped %d" % (
        len(run.nodeproc), len(run.proxies), run.nodes, m["ppa"], m["pxsend"], m["order"], m["shift"], m["round_mb"], m["sample"], sum(h["nrec"] for h in run.files), sum(h["dropped"] for h in run.nodeproc + run.proxies)))
    if any(h["dropped"] for h in run.files):
        L.append("  WARNING: some records were dropped (arena full): the tail of the run is missing there; raise COMM_TRACE_MAX_MB or COMM_TRACE_SAMPLE")
    env = m.get("env", {}); keys = [k for k in sorted(env) if k.startswith("COMM_") or k.startswith("MN_")]
    L.append("  switches: " + " ".join("%s=%s" % (k, env[k]) for k in keys if k not in ("COMM_TRACE_DIR",))[:900])
    L.append("  clock (node j minus node 0, us): node | off init | off end | drift us/s | best rtt us | median rtt us | bound +-us")
    for j in sorted(run.clock):
        o0, x0, o1, x1, rtt, med = run.clock[j]
        drift = (o1 - o0) / ((x1 - x0) / 1e9) / 1e3 if o1 is not None and x1 != x0 else NAN
        L.append("   %3d | %10.1f | %10s | %9.3f | %7.1f | %7.1f | %6.1f" % (j, o0 / 1e3, "%.1f" % (o1 / 1e3) if o1 is not None else "-", drift, rtt / 1e3, med / 1e3, rtt / 2e3))
    if not run.clock:
        L.append("   (no clock table: one node, or node 0's file is missing; cross-node latencies are not valid)")


def section_volume(acc, L):
    L.append("2 volume per exchange kind and path (exchange-ranks = one rank's exchange; messages = one sender -> receiver of one round):")
    L.append("   kind    path    exch-ranks    messages   send GB (sum of E_BEGIN)   msg GB   KiB/msg mean  min  max")
    for (kd, pa), a in sorted(acc.vol.items()):
        L.append("   %-7s %-7s %9d %11d %14.2f %17.2f %9.1f %9.1f %9.1f" % (KIND[kd] if kd < len(KIND) else kd, PATH[pa] if pa < 3 else pa, a[0], a[1], a[2] / 1e9, a[3] / 1e9,
                                                                      a[3] / a[1] / 1024 if a[1] else 0, (a[4] if a[1] else 0) / 1024, a[5] / 1024))


def section_stages(S, L, title="3 stage latency"):
    L.append("%s (exchange and round stages in ms, message stages in us; n | p50 p90 p99 max mean):" % title)
    hdr = "   %-6s %-12s %-7s %7s %9s %9s %9s %9s %9s  %s" % ("layer", "stage", "kind", "n", "p50", "p90", "p99", "max", "mean", "neg%")
    L.append(hdr)
    for (layer, name, kd), p in sorted(S.items(), key=lambda x: (["ex", "rd", "msg"].index(x[0][0]), x[0][1] if x[0][0] != "msg" else MSG_STAGES.index(x[0][1]) if x[0][1] in MSG_STAGES else 99, x[0][2])):
        if p is None:
            continue
        L.append("   %-6s %-12s %-7s %s  %s" % (layer, name, "all" if kd < 0 else KIND[kd], fmtp(p), "%.0f" % p["neg"] if p["neg"] else ""))


def section_links(run, acc, L):
    if not acc.msgs:
        return
    M = {k: np.concatenate([x[k] for x in acc.msgs]) for k in acc.msgs[0]}
    L.append("4 links (messages with a signal seen; e2e = put issued -> signal seen, cross-node; sig1 = signal one way; rate = bytes / e2e per message):")
    L.append("   src->dst    msgs  e2e us p50    p90      max   sig1 us p50    p99   rate GB/s p50   sum GB")
    keys = np.unique(M["nsrc"].astype(np.int64) * 1000 + M["ndst"])
    for k in keys:
        s = (M["nsrc"] * 1000 + M["ndst"]) == k; e = M["e2e"][s]; e = e[np.isfinite(e)]; sg = M["sig"][s]; sg = sg[np.isfinite(sg)]; r = (M["bytes"][s] / np.where(M["e2e"][s] > 0, M["e2e"][s], NAN)); r = r[np.isfinite(r)]
        if len(e) == 0:
            continue
        L.append("   %2d->%-2d %9d %9.0f %9.0f %9.0f %9.1f %9.1f %9.2f %11.1f" % (k // 1000, k % 1000, int(s.sum()), np.median(e) / 1e3, np.percentile(e, 90) / 1e3, e.max() / 1e3,
                                                                           np.median(sg) / 1e3 if len(sg) else NAN, np.percentile(sg, 99) / 1e3 if len(sg) else NAN, np.median(r) if len(r) else NAN, M["bytes"][s].sum() / 1e9))
    L.append("   peer distance (dst rank - src rank mod n; the shift order's step k):")
    for k in np.unique(M["dist"]):
        s = M["dist"] == k; e = M["e2e"][s]; e = e[np.isfinite(e)]; sg = M["sig"][s]; sg = sg[np.isfinite(sg)]; rw = M["rw"][s]; rw = rw[np.isfinite(rw)]
        if len(e):
            L.append("   k=%-2d %9d  e2e us p50 %9.0f p90 %9.0f max %9.0f | sig1 p50 %8.1f | roff wait us p50 %8.0f p90 %9.0f" % (k, int(s.sum()), np.median(e) / 1e3, np.percentile(e, 90) / 1e3, e.max() / 1e3,
                                                                                                    np.median(sg) / 1e3 if len(sg) else NAN, np.median(rw) / 1e3 if len(rw) else NAN, np.percentile(rw, 90) / 1e3 if len(rw) else NAN))


def section_last(run, acc, L):
    L.append("5 who is last (node = node index; lag/gap in us):")
    def tab(rows, title, ix_node=0, ix_val=1):
        if not rows:
            L.append("   %s: no data" % title); return
        cnt = collections.Counter(r[ix_node] for r in rows); tot = len(rows)
        L.append("   %s (%d cases; uniform would be %.1f%% per node):" % (title, tot, 100.0 / max(1, run.nodes)))
        for nd, c in sorted(cnt.items()):
            v = np.array([r[ix_val] for r in rows if r[ix_node] == nd]) / 1e3
            L.append("     node %2d: %6.1f%%  n %-7d lag p50 %9.1f  p90 %9.1f  max %10.1f" % (nd, 100.0 * c / tot, c, np.median(v), np.percentile(v, 90), v.max()))
    tab(acc.last["recv_last"], "receiver: sender node seen last (value = gap to the next-to-last signal seen)", 0, 2)
    rows = acc.last["recv_last"]
    if rows:
        cnt = collections.Counter(r[1] for r in rows)
        L.append("     by peer distance of the last-seen sender: " + "  ".join("k=%d: %.1f%%" % (k, 100.0 * c / len(rows)) for k, c in sorted(cnt.items())))
    tab(acc.last["ex_issue"], "exchange: node that issued its last signal last (lag vs the median node's last issue)", 0, 1)
    tab(acc.last["ex_pub"], "exchange: node that published its offset word last (lag vs the median receiver)", 0, 1)
    tab(acc.last["end_roff_wait"], "sender waits: receiver node whose offset word was awaited longest per sender and exchange (value = that wait)", 0, 2)
    tab(acc.last["end_sig_wait"], "receiver waits: sender node whose signal was awaited longest per receiver and exchange (value = that wait)", 0, 2)


def fmt_hdr_ex(run, cid, x):
    return "comm %d xseq %d" % (cid, x)


def section_slowest(run, acc, L, P, nshow, ntl):
    L.append("6 slowest exchanges by exchange time (xt = last done - last begin, i.e. after the last rank arrived: transport, copies, waits; the arrival skew is listed with each and ranked in 6b; stage values ms on each rank):")
    top = acc.top[:nshow]
    for rank_i, (dur, cid, xs, sub) in enumerate(top):
        cinfo = run.comms[cid]; E = run.ev
        ex = ex_table(sub, E); mt = msg_table(sub, E, cinfo["node"], cinfo["apu"], P)
        t0 = float(np.nanmin(ex["t_begin"])) if ex is not None else float(sub["t"].min())
        k = int(ex["K"].max()) if ex is not None else 1
        L.append(" #%d comm %s (APU %d) xseq %d kind %s path %s rounds %d: xt %.2f ms; arrival skew %.2f ms" % (rank_i + 1, cinfo["lab"], cinfo["apu"], xs, KIND[int(ex["kind"][0])] if ex is not None else "?", PATH[int(ex["path"].max())] if ex is not None else "?", k,
                                                                                                          dur / 1e6, (np.nanmax(ex["t_begin"]) - t0) / 1e6 if ex is not None else NAN))
        if ex is not None:
            L.append("     rank node | begin  post  call   join  sigs  quiet rcopy | done (ms from the first begin) | send MiB")
            for i in np.argsort(ex["me"]):
                f = lambda a: (a - 0) / 1e6
                L.append("     %4d %4d | %6.2f %5.2f %5.2f %6.2f %5.2f %6.2f %5.2f | %7.2f | %8.1f" % (ex["me"][i], cinfo["node"][int(ex["me"][i])], (ex["t_begin"][i] - t0) / 1e6, f(ex["t_posted"][i] - ex["t_begin"][i]),
                                                                                                    f(ex["t_wait"][i] - ex["t_posted"][i]), f(ex["t_joined"][i] - ex["t_wait"][i]), f(ex["t_sigs"][i] - ex["t_joined"][i]),
                                                                                                    f(ex["t_quiet"][i] - ex["t_sigs"][i]), f(ex["t_rcopy"][i] - ex["t_rciss"][i]), (ex["t_done"][i] - t0) / 1e6, ex["bytes"][i] / 1048576.0))
        if mt is not None:
            m, d = mt; o = np.argsort(-np.nan_to_num(d["e2e"], nan=-1))[:6]
            L.append("     slowest messages (us): src->dst (nodes) rnd MiB | roffwait post wire/pxbusy sig1 e2e | roff pub -> put (us)")
            for i in o:
                L.append("       %2d->%-2d (%d->%d) r%-3d %6.1f | %9.0f %6.0f %9.0f %7.0f %9.0f | %s" % (m["src"][i], m["dst"][i], m["nsrc"][i], m["ndst"][i], m["rnd"][i], m["bytes"][i] / 1048576.0, d["roff_wait"][i] / 1e3, d["put_post"][i] / 1e3,
                                                                                                   (d["wire"][i] if np.isfinite(d["wire"][i]) else d["px_busy"][i]) / 1e3, d["sig_oneway"][i] / 1e3, d["e2e"][i] / 1e3,
                                                                                                   "%.0f" % ((m["t_put"][i] - m["t_roffi"][i]) / 1e3) if np.isfinite(m["t_roffi"][i]) else "-"))
        if rank_i < ntl:
            L.append("     raw timeline (ms from first begin; node rank event peer round bytes aux), first 160 events:")
            o = np.argsort(sub["t"], kind="stable")[:160]
            names = {v: k for k, v in run.ev.items()}
            for r in sub[o]:
                L.append("       %9.3f n%-2d r%-2d %-14s peer %-3d rd %-3d %10d %s" % ((int(r["t"]) - t0) / 1e6, cinfo["node"][int(r["me"])] if 0 <= r["me"] < len(cinfo["node"]) else -1, r["me"], names.get(int(r["ev"]), "?"), r["peer"], r["round"], r["bytes"], r["aux"] if r["aux"] != PXNONE else ""))


def section_time(run, acc, L, binw, ratio):
    L.append("7 clustering in time (slow = exchange time xt > %.1f x the median of its kind / path / size class; bins of %.1f s of exchange start on node 0's clock):" % (ratio, binw))
    if not acc.exs:
        L.append("   no exchanges"); return
    E = {k: np.concatenate([e[k] for e in acc.exs]) for k in acc.exs[0]}
    dur = E["xt"]; ok = np.isfinite(dur) & (dur > 0)             # the exchange time after the last arrival (the arrival skew is separate: see 6b)
    sk = E["skew"][np.isfinite(E["skew"])]
    L.append("   arrival skew (last begin - first begin) ms: p50 %.3f p90 %.3f p99 %.3f max %.3f; exchange time after the last arrival (xt) ms: p50 %.3f p90 %.3f p99 %.3f max %.3f" % (
        *(np.percentile(sk, [50, 90, 99]) / 1e6), sk.max() / 1e6, *(np.percentile(dur[ok], [50, 90, 99]) / 1e6), dur[ok].max() / 1e6))
    cls = E["kind"] * 1000 + E["path"] * 100 + np.floor(np.log2(np.maximum(E["bytes"], 1))).astype(np.int64)
    med = {}
    for c in np.unique(cls[ok]):
        med[c] = np.median(dur[ok & (cls == c)])
    r = np.array([dur[i] / med[cls[i]] if ok[i] and cls[i] in med else NAN for i in range(len(dur))])
    slow = np.isfinite(r) & (r > ratio)
    L.append("   exchanges %d, slow %d (%.2f%%); duration ms: p50 %.2f p90 %.2f p99 %.2f max %.2f" % (len(dur), int(slow.sum()), 100.0 * slow.mean(), *(np.percentile(dur[ok], [50, 90, 99]) / 1e6), dur[ok].max() / 1e6))
    tsec = E["t_start"] / 1e9; tb = np.floor((tsec - np.nanmin(tsec)) / binw).astype(np.int64); nb = int(tb.max()) + 1
    tot = np.bincount(tb, minlength=nb); sl = np.bincount(tb[slow], minlength=nb)
    mean = sl.mean(); fano = sl.var() / mean if mean > 0 else NAN
    L.append("   per-bin slow count: mean %.2f, variance/mean (Fano; 1 = spread like chance, >> 1 = clustered) %.2f; bins with slow>0: %d of %d" % (mean, fano, int((sl > 0).sum()), nb))
    if slow.any():
        srt = np.sort(sl)[::-1]; top5 = max(1, int(round(0.05 * nb)))
        L.append("   share of slow exchanges in the worst 5%% of bins: %.1f%% (spread would be ~5%%)" % (100.0 * srt[:top5].sum() / sl.sum()))
        # persistence per communicator: P(slow | previous exchange of the same communicator slow) vs P(slow)
        pp = [];
        for cid in np.unique(E["comm"]):
            s = E["comm"] == cid; o = np.argsort(E["xseq"][s]); x = slow[s][o]
            if len(x) > 2:
                pp.append((int((x[1:] & x[:-1]).sum()), int(x[:-1].sum()), len(x) - 1))
        a = sum(p[0] for p in pp); b = sum(p[1] for p in pp); n = sum(p[2] for p in pp); p0 = slow.mean()
        L.append("   persistence: P(slow | previous exchange of the communicator slow) = %.2f vs P(slow) = %.2f (ratio %.1f; 1 = independent)" % (a / b if b else NAN, p0, (a / b) / p0 if b and p0 else NAN))
        o = np.argsort(-sl)[:8]
        L.append("   worst bins: start s | slow / exchanges | slowest ms")
        for b_ in sorted(o):
            if sl[b_]:
                s = tb == b_
                L.append("     %8.1f | %4d / %-5d | %8.2f" % (b_ * binw, sl[b_], tot[b_], dur[s].max() / 1e6))
        # which node is last in the slow exchanges: by the last-begin skew (entry) of the exchange
        L.append("   slow exchanges (by xt): median entry skew %.2f ms (all: %.2f ms); median duration %.2f ms (all: %.2f ms)" % (np.nanmedian(E["skew"][slow]) / 1e6, np.nanmedian(E["skew"][ok]) / 1e6, np.median(dur[slow]) / 1e6, np.median(dur[ok]) / 1e6))
    # messages
    if acc.msgs:
        M = {k: np.concatenate([x[k] for x in acc.msgs]) for k in acc.msgs[0]}
        e2e = M["e2e"].astype(np.float64); okm = np.isfinite(e2e) & (e2e > 0)
        cls = M["kind"].astype(np.int64) * 100 + np.floor(np.log2(np.maximum(M["bytes"], 1))).astype(np.int64); med = {}
        for c in np.unique(cls[okm]):
            med[c] = np.median(e2e[okm & (cls == c)])
        ratio_m = np.array([e2e[i] / med[cls[i]] if okm[i] and cls[i] in med else NAN for i in range(len(e2e))]); mr = max(ratio, 3.0)
        sl = np.isfinite(ratio_m) & (ratio_m > mr)
        L.append("   messages %d%s, slow (e2e > %.1f x its kind/size median) %d (%.2f%%)" % (len(e2e), " (thinned to %.0f%%)" % (100 * acc.keep) if acc.keep < 1 else "", mr, int(sl.sum()), 100.0 * sl.mean()))
        if sl.any():
            for who, key in (("sender", "nsrc"), ("receiver", "ndst")):
                cnt = np.bincount(M[key][sl].astype(np.int64), minlength=run.nodes); tot_ = np.bincount(M[key].astype(np.int64), minlength=run.nodes)
                L.append("   slow messages by %s node (%% of the node's messages): %s" % (who, "  ".join("n%d %.2f%%" % (i, 100.0 * cnt[i] / tot_[i]) for i in range(run.nodes) if tot_[i])))
            t = M["t_put"][sl] / 1e9; tb2 = np.floor((t - np.nanmin(M["t_put"] / 1e9)) / binw).astype(np.int64); nb2 = int(np.floor((np.nanmax(M["t_put"] / 1e9) - np.nanmin(M["t_put"] / 1e9)) / binw)) + 1
            c2 = np.bincount(tb2, minlength=nb2); mean = c2.mean(); fano = c2.var() / mean if mean else NAN
            L.append("   slow messages per bin: Fano %.1f; worst bins (start s: slow msgs): %s" % (fano, "  ".join("%.0f: %d" % (b * binw, c2[b]) for b in np.argsort(-c2)[:6] if c2[b])))
            # a stalled node: bins where one sender node holds most slow messages
            L.append("   (a node stall shows as a bin whose slow messages all share one sender or receiver node; compare the per-node shares above)")


def section_skew(run, acc, L):
    L.append("6b slowest exchanges by arrival skew (last begin - first begin over the ranks; late = node whose rank began last; xt = exchange time after the last arrival; times in s / ms on node 0's clock):")
    L.append("   skew ms |  xt ms | comm xseq kind | late node | first node | first begin s | MiB sent (sum)")
    t0 = run.t_first
    for x in acc.topskew:
        L.append("   %8.1f | %6.2f | %s %d %s | n%-2d | n%-2d | %9.3f | %9.1f" % (x["skew"] / 1e6, x["xt"] / 1e6, run.comms[x["cid"]]["lab"], x["xseq"], KIND[x["kind"]] if x["kind"] < len(KIND) else x["kind"], x["late"], x["first"], (x["t_start"] - t0) / 1e9, x["bytes"] / 1048576.0))
    if acc.exs:
        E = {k: np.concatenate([e[k] for e in acc.exs]) for k in ("skew", "late_node", "xt")}
        big = E["skew"] > 50e6
        L.append("   exchanges with skew > 50 ms: %d of %d; by late node: %s" % (int(big.sum()), len(E["skew"]), "  ".join("n%d %d" % (n, int((E["late_node"][big] == n).sum())) for n in range(run.nodes) if (E["late_node"][big] == n).any()) or "-"))


HB_KEY = ["compact_stall", "compact_fail", "thp_fault_alloc", "thp_fault_fallback", "thp_collapse_alloc", "pgmajfault", "pswpin", "pswpout", "allocstall", "numa_pages_migrated", "numa_hint_faults", "pgscan_direct", "pgsteal_direct",
          "psi_cpu_some_us", "psi_mem_some_us", "psi_mem_full_us", "psi_io_some_us", "psi_io_full_us", "thr_nivcsw", "thr_majflt", "proc_nivcsw", "proc_majflt"]


def load_hb(run):
    """heartbeat records (COMM_TRACE_HB): {file index: dict(node, proc, gaps [(t_end, gap_ns, snap)], snaps {id: (t, kind, {name: value})})}; times on node 0's clock"""
    out = []
    if "H_GAP" not in run.ev:
        return out
    eg, ec = run.ev["H_GAP"], run.ev["H_CNT"]
    ep, et, ew = run.ev.get("H_PHASE", 255), run.ev.get("H_THR", 255), run.ev.get("H_WD", 255)      # (v2 records; absent in older traces)
    for h in run.files:
        names = h["meta"].get("hbnames") or []
        if not names or h["nrec"] == 0:
            continue
        node = h["meta"]["node"]; gaps = []; snaps = {}; phraw = collections.defaultdict(list); thr = collections.defaultdict(list); wds = []
        strs = h["meta"].get("hbstrs") or []; ss = lambda i: strs[i] if 0 <= i < len(strs) else "?"
        for c in chunks(h):
            m = (c["ev"] == eg) | (c["ev"] == ec) | (c["ev"] == ep) | (c["ev"] == et) | (c["ev"] == ew)
            if not m.any():
                continue
            c = c[m]; t = to_clock0(run, c["t"], node)
            for r, tt in zip(c, t):
                if r["ev"] == ep:
                    phraw[int(r["comm"])].append((float(tt), int(r["aux"])))
                elif r["ev"] == et:
                    b = int(r["bytes"]); thr[int(r["comm"])].append(dict(tid=int(r["seq"]), wchan=ss(int(r["xseq"])), name=ss(int(r["peer"])), age=int(r["aux"]), ph=int(r["round"]), cpu=int(r["me"]),
                                                                       kind=int(r["kp"]) & 3, me=bool(int(r["kp"]) & 128), delay=(b & 0x00ffffffffffffff) / 1e6, state=chr(b >> 56) if b >> 56 else "?"))
                elif r["ev"] == ew:
                    wds.append((float(tt), float(r["bytes"]), int(r["aux"])))
                elif r["ev"] == eg:
                    gaps.append((float(tt), float(r["bytes"]), int(r["aux"])))
                else:
                    sn = snaps.setdefault(int(r["aux"]), [float(tt), int(r["seq"]), {}])
                    sn[2][names[int(r["round"])] if int(r["round"]) < len(names) else "?%d" % int(r["round"])] = int(r["bytes"])
        if gaps or snaps:
            phtl = {tid: ([x[0] for x in sorted(v)], [x[1] for x in sorted(v)]) for tid, v in phraw.items()}
            out.append(dict(node=node, proc=("px%d" % h["meta"]["apu"]) if h["meta"]["proxy"] else "np", gaps=sorted(gaps), snaps=snaps, phtl=phtl, thr=thr, wds=sorted(wds), phnames=h["meta"].get("phases") or [], gap_ms=h["meta"].get("hb_gap_ms"), hb_ms=h["meta"].get("hb_ms")))
    return out


def hb_delta(hb, t_a, snap_b):
    """counter deltas between the last snapshot that ended before t_a and snapshot snap_b (the gap's own)"""
    sb = hb["snaps"].get(snap_b)
    if sb is None:
        return None
    prev = None
    for i, sn in hb["snaps"].items():
        if i != snap_b and sn[0] <= t_a and (prev is None or sn[0] > prev[0]):
            prev = sn
    if prev is None:
        return None
    d = {}
    for k, v in sb[2].items():
        if k in prev[2] and v != prev[2][k] and (k in HB_KEY or k.startswith("allocstall") or k.startswith("compact_") or k.startswith("numa_")):
            d[k] = v - prev[2][k]
    return d, (t_a - prev[0]) / 1e9


def fmt_delta(d, lim=9):
    if d is None:
        return "(no earlier snapshot)"
    items = sorted(d.items(), key=lambda kv: -abs(kv[1]))[:lim]
    return " ".join("%s+%d" % (k.replace("_us", "us"), v) for k, v in items) or "no counter moved"


def phase_at(h, t):
    """the phase tag of every thread of the process at time t (node 0's clock), from the H_PHASE timeline: [(tid, tag name, ms since it was set)]"""
    out = []; pn = h["phnames"]
    for tid in sorted(h["phtl"]):
        ts, tags = h["phtl"][tid]; i = bisect.bisect_right(ts, t) - 1
        if i >= 0:
            out.append((tid, pn[tags[i]] if tags[i] < len(pn) else "?%d" % tags[i], (t - ts[i]) / 1e6))
    return out


def fmt_phases(ph):
    return " ".join("t%d:%s(%.0fms)" % (tid, nm, ms) for tid, nm, ms in ph) or "(no phase events)"


def fmt_thr(h, snap):
    """the per-thread snapshot: tid name state wchan phase(age) run_delay cpu; the heartbeat thread itself is left out"""
    rows = sorted(h["thr"].get(snap, []), key=lambda r: r["tid"]); pn = h["phnames"]; out = []
    for r in rows:
        if r["me"]:
            continue
        out.append("%d/%s:%s,%s,%s(%dms),dly %.1fms,cpu%d" % (r["tid"], r["name"], r["state"], r["wchan"], pn[r["ph"]] if r["ph"] < len(pn) else "?%d" % r["ph"], r["age"], r["delay"], r["cpu"]))
    return "; ".join(out) or "(no thread snapshot)"


def section_stall_phases(run, acc, L, hbs):
    """8b: the watchdog firings and the slowest exchanges' late nodes, each with the phase tag of every thread of the node-process, the thread snapshot (state, wchan, run_delay) and the heartbeat state"""
    nps = [h for h in hbs if h["proc"] == "np"]; t0 = run.t_first
    L.append("8b stall classification (COMM_TRACE_PH phase tags, COMM_TRACE_THR thread snapshots, COMM_TRACE_WD_MS progress watchdog; node-process only):")
    if not any(h["phtl"] or h["wds"] or h["thr"] for h in nps):
        L.append("   no phase, thread-snapshot or watchdog records"); return
    L.append("   per node: watchdog firings, phase events, thread snapshots: " + "  ".join("n%d %d/%d/%d" % (h["node"], len(h["wds"]), sum(len(v[0]) for v in h["phtl"].values()), len(h["thr"])) for h in nps))
    wl = sorted([(w[1], h, w) for h in nps for w in h["wds"]], key=lambda x: -x[0])[:12]; wl.sort(key=lambda x: x[2][0])
    L.append("   the %d longest watchdog firings in time order (idle = no exchange / round / message record on the node-process for that long; onset = the time the last record was stamped):" % len(wl))
    for idle, h, w in wl:
        on = w[0] - w[1]; gp = [g for g in h["gaps"] if g[0] >= on and g[0] - g[1] <= w[0]]
        L.append("   %9.3f | n%-2d | idle %8.1f ms | heartbeat: %s" % ((w[0] - t0) / 1e9, h["node"], idle / 1e6, "gaps %s ms" % ",".join("%.0f" % (g[1] / 1e6) for g in gp[:6]) if gp else "alive (no gap)"))
        L.append("       phases at onset: %s" % fmt_phases(phase_at(h, on)))
        L.append("       snapshot: %s" % fmt_thr(h, w[2]))
    L.append("   the slowest exchanges by skew: the late node's threads just before its late begin (phase tags from the timeline), the watchdog firing and heartbeat gap in the window:")
    for x in acc.topskew[:10]:
        h = next((h for h in nps if h["node"] == x["late"]), None)
        if h is None:
            continue
        lo, hi = x["t_last"] - x["skew"], x["t_last"] + 20e6
        wd = [w for w in h["wds"] if lo <= w[0] <= hi]; gp = [g for g in h["gaps"] if g[0] >= lo and g[0] - g[1] <= hi]
        L.append("     skew %8.1f ms comm %s xseq %d late n%d at %.3f s: heartbeat %s; watchdog %s" % (x["skew"] / 1e6, run.comms[x["cid"]]["lab"], x["xseq"], x["late"], (x["t_last"] - t0) / 1e9,
                 "gaps %s ms" % ",".join("%.0f" % (g[1] / 1e6) for g in gp[:6]) if gp else "alive (no gap)", "%d firing(s), idle up to %.0f ms" % (len(wd), max(w[1] for w in wd) / 1e6) if wd else "none"))
        L.append("         phases at the late begin - 1 ms: %s" % fmt_phases(phase_at(h, x["t_last"] - 1e6)))
        if wd:
            L.append("         snapshot of the first firing: %s" % fmt_thr(h, wd[0][2]))


def section_storms(run, L, hbs, thresh=20000, maxn=40):
    """8a (COMM_TRACE_HB_MEM=1): per node, the seconds where pgmigrate_fail exceeds `thresh` per second (a compaction storm), with the second before and after:
    fragmentation score per NUMA node (Normal zone: 100 * free pages in blocks < 2 MiB / free pages), MemFree, Dirty+Writeback, kcompactd / khugepaged CPU ticks and KFD evicted_ms moved in the interval"""
    L.append("8a compaction storms (pgmigrate_fail > %d/s per node, periodic snapshots; COMM_TRACE_HB_MEM=1 adds frag, MemFree, Dirty+Writeback, kcompactd ticks, KFD evicted_ms):" % thresh)
    t0 = run.t_first; shown = 0; found = 0; mem = False
    for h in sorted(hbs, key=lambda x: x["node"]):
        if h["proc"] != "np":
            continue
        sn = sorted([v for v in h["snaps"].values() if v[1] == 1], key=lambda v: v[0])
        mem = mem or any("kfd_present" in v[2] or any(k.startswith("mi_") for k in v[2]) for v in sn[:1])
        hot = [i for i in range(1, len(sn)) if "pgmigrate_fail" in sn[i][2] and "pgmigrate_fail" in sn[i - 1][2]
               and (sn[i][2]["pgmigrate_fail"] - sn[i - 1][2]["pgmigrate_fail"]) / max((sn[i][0] - sn[i - 1][0]) / 1e9, 1e-3) > thresh]
        if not hot:
            continue
        found += len(hot); L.append("   node %d: %d storm second(s)" % (h["node"], len(hot)))
        rows = sorted(set(j for i in hot for j in (i - 1, i, i + 1) if 1 <= j < len(sn)))
        for j in rows:
            if shown >= maxn:
                break
            a, b = sn[j - 1][2], sn[j][2]; dt = max((sn[j][0] - sn[j - 1][0]) / 1e9, 1e-3)
            d = lambda k: (b[k] - a[k]) if k in a and k in b else None
            pf = d("pgmigrate_fail"); fr = sorted((k, v) for k, v in b.items() if k.startswith("bud_n") and k.endswith("_Normal_frag"))
            frs = " ".join("%s:%d" % (k[5:k.index("_Normal")], v) for k, v in fr) or "-"
            sm = lambda ks: (sum(x for x in ks if x is not None) if any(x is not None for x in ks) else None)
            kc = sm([d(k) for k in b if k.startswith("kcompactd") and k.endswith("_ticks")]); kh = d("khugepaged_ticks")
            ev = sm([d(k) for k in b if k.startswith("kfd_") and k.endswith("evicted_ms")])
            wb = sm([b.get("mi_Dirty_kb"), b.get("mi_Writeback_kb")])
            L.append("     %9.3f s %s| pgmigrate_fail %8s/s | frag %s | MemFree %s | Dirty+WB %s | kcompactd %s ticks khugepaged %s | KFD evicted_ms %s" % (
                (sn[j][0] - t0) / 1e9, "*" if j in hot else " ", "%d" % (pf / dt) if pf is not None else "-", frs,
                "%.1f GB" % (b["mi_MemFree_kb"] / 1048576) if "mi_MemFree_kb" in b else "-", "%.1f MB" % (wb / 1024) if wb is not None else "-",
                kc if kc is not None else "-", kh if kh is not None else "-", ("+%d" % ev) if ev is not None else ("absent" if b.get("kfd_present") == 0 else "-")))
            shown += 1
    if not found:
        L.append("   no storm second found")
    elif not mem:
        L.append("   (no memory counters in this trace: COMM_TRACE_HB_MEM was off)")


def section_stalls(run, acc, L, hbs):
    L.append("8 stalls: heartbeat wake-up gaps (COMM_TRACE_HB) joined with OS counters and the late node of the exchanges (times s on node 0's clock):")
    if not hbs:
        L.append("   no heartbeat records (COMM_TRACE_HB unset, or an older trace)"); return
    h0 = hbs[0]; L.append("   heartbeat every %s ms, gap threshold %s ms; threads: %d (%s); gaps seen: %d" % (h0["hb_ms"], h0["gap_ms"], len(hbs), ",".join(sorted(set(h["proc"] for h in hbs))), sum(len(h["gaps"]) for h in hbs)))
    L.append("   per node: gaps (heartbeat thread of the node-process / of the proxies), longest ms, counter totals over the run (last - first snapshot of the node-process):")
    for nd in range(run.nodes):
        mine = [h for h in hbs if h["node"] == nd]
        if not mine:
            continue
        gnp = sum(len(h["gaps"]) for h in mine if h["proc"] == "np"); gpx = sum(len(h["gaps"]) for h in mine if h["proc"] != "np")
        lg = max([g[1] for h in mine for g in h["gaps"]] or [0]) / 1e6
        tot = ""
        for h in mine:
            if h["proc"] == "np" and len(h["snaps"]) >= 2:
                ids = sorted(h["snaps"], key=lambda i: h["snaps"][i][0]); a, b = h["snaps"][ids[0]][2], h["snaps"][ids[-1]][2]
                tot = fmt_delta({k: b[k] - a[k] for k in b if k in a and b[k] != a[k] and (k in HB_KEY or k.startswith("allocstall") or k.startswith("compact_") or k.startswith("numa_"))}, 12)
        L.append("     node %2d: gaps np %4d px %4d  longest %9.1f ms | %s" % (nd, gnp, gpx, lg, tot))
    t0 = run.t_first
    allg = sorted([(g[1], g[0], h, g) for h in hbs for g in h["gaps"]], key=lambda x: -x[0])[:30]
    allg.sort(key=lambda x: x[1])
    E = {k: np.concatenate([e[k] for e in acc.exs]) for k in ("t_start", "t_last", "skew", "late_node", "xt", "kind")} if acc.exs else None
    L.append("   the %d longest gaps in time order: time s | node proc | gap ms | procs of the node with a gap overlapping | OS counters moved since the previous snapshot | late exchanges (node = late node, begin within the gap +-50 ms): n, max skew ms" % len(allg))
    for gap, tend, h, g in allg:
        ta = g[0] - g[1]; dl = hb_delta(h, ta, g[2])
        over = sorted(set(h2["proc"] for h2 in hbs if h2["node"] == h["node"] for g2 in h2["gaps"] if g2[0] - g2[1] <= g[0] and g2[0] >= ta))
        late = ""
        if E is not None:
            s = (E["late_node"] == h["node"]) & (E["t_last"] >= ta - 50e6) & (E["t_last"] <= g[0] + 50e6) & (E["skew"] > 2e6)
            late = "%d, %.1f" % (int(s.sum()), E["skew"][s].max() / 1e6) if s.any() else "0"
        L.append("   %9.3f | n%-2d %-3s | %8.1f | %-14s | %s%s | %s" % ((g[0] - t0) / 1e9, h["node"], h["proc"], g[1] / 1e6, ",".join(over), fmt_delta(dl[0] if dl else None), " (over %.1fs)" % dl[1] if dl else "", late))
    L.append("   the slowest exchanges by skew against the heartbeat of their late node (a gap that ended between the first and the last begin):")
    for x in acc.topskew[:10]:
        hit = [(h["proc"], g) for h in hbs if h["node"] == x["late"] for g in h["gaps"] if g[0] >= x["t_start"] and g[0] - g[1] <= x["t_last"] + 20e6]
        if hit:
            best = max(hit, key=lambda z: z[1][1]); dl = None
            for h in hbs:
                if h["node"] == x["late"] and h["proc"] == best[0]:
                    dl = hb_delta(h, best[1][0] - best[1][1], best[1][2])
            L.append("     skew %8.1f ms comm %s xseq %d late n%d: gap %.1f ms on %s at %.3f s; %s" % (x["skew"] / 1e6, run.comms[x["cid"]]["lab"], x["xseq"], x["late"], best[1][1] / 1e6, best[0], (best[1][0] - t0) / 1e9, fmt_delta(dl[0] if dl else None)))
        else:
            L.append("     skew %8.1f ms comm %s xseq %d late n%d: no heartbeat gap on that node in the window" % (x["skew"] / 1e6, run.comms[x["cid"]]["lab"], x["xseq"], x["late"]))


def run_report(run, args, L):
    tmp = tempfile.mkdtemp(prefix="trace_report_", dir=args.tmp)
    try:
        P = load_proxies(run)
        counts = spill(run, tmp)
        acc = Acc(args.max_msgs); rng = np.random.default_rng(1)
        for cid in sorted(counts, key=lambda c: -counts[c]):
            if cid not in run.comms:
                continue
            R = np.fromfile(os.path.join(tmp, "c%d.bin" % cid), dtype=REC2)
            add_comm(R, run, cid, run.comms[cid], P, acc, rng, args.top)
            acc.ncomm += 1
            del R
        S = stage_summary(acc)
        section_header(run, L)
        L.append("   communicators with records: %d; exchanges %d; messages %d%s" % (acc.ncomm, acc.nex, acc.nmsg, "; message tables thinned to %.0f%% beyond --max-msgs" % (100 * acc.keep) if acc.keep < 1 else ""))
        section_volume(acc, L)
        section_stages(S, L)
        section_links(run, acc, L)
        section_last(run, acc, L)
        run.t_first = float(np.nanmin(np.concatenate([e["t_start"] for e in acc.exs]))) if acc.exs else 0.0
        section_slowest(run, acc, L, P, args.top, args.timeline)
        section_skew(run, acc, L)
        section_time(run, acc, L, args.bin_s, args.slow_ratio)
        hbs = load_hb(run); section_stalls(run, acc, L, hbs); section_storms(run, L, hbs); section_stall_phases(run, acc, L, hbs)
        L.append("")
        return S
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def compare(runs, sums, L):
    L.append("== COMPARISON of %s ==" % ", ".join(r.label for r in runs))
    L.append("   p50 / p90 / p99 per stage and kind (ms for ex / rd, us for msg)")
    keys = sorted(set(k for s in sums for k in s), key=lambda k: (["ex", "rd", "msg"].index(k[0]), k[1] if k[0] != "msg" else MSG_STAGES.index(k[1]) if k[1] in MSG_STAGES else 99, k[2]))
    L.append("   %-5s %-12s %-6s | " % ("layer", "stage", "kind") + " | ".join("%-26s" % r.label[:26] for r in runs))
    for k in keys:
        if k[2] < 0 and any(((k[0], k[1], kd) in s) for s in sums for kd in range(len(KIND))) and False:
            continue
        cells = []
        for s in sums:
            p = s.get(k)
            cells.append("%-26s" % ("%.3f / %.3f / %.3f" % (p["p50"], p["p90"], p["p99"]) if p else "-"))
        L.append("   %-5s %-12s %-6s | " % (k[0], k[1], "all" if k[2] < 0 else KIND[k[2]]) + " | ".join(cells))


# ---------------------------------------------------------------- selftest: a synthetic trace
def synth(d, nodes=3, nex=60, seed=3):
    """one communicator (id 4, APU 0), rounds path K = 1 with PX3 proxies; node 2 is the slow one (late by 4 ms in 10% of the exchanges, in a burst);
    node clocks: node j = true + j*1.5 ms + drift j*40 us/s"""
    rng = np.random.default_rng(seed); EVN = ["E_BEGIN", "E_COPY_ISS", "E_COPY_DONE", "E_PUBLISHED", "E_POSTED", "E_WAIT", "E_JOINED", "E_SIGS_SEEN", "E_QUIET_END", "E_RCOPY_ISS", "E_RCOPY_DONE", "E_DONE",
                                           "R_BEGIN", "R_PUBLISHED", "R_SCOPY_ISS", "R_SCOPY_DONE", "R_SENDS_DONE", "R_SIGS_DONE", "R_QUIET_END", "R_RCOPY_ISS", "R_END",
                                           "M_ROFF_ISSUE", "M_ROFF_WAIT", "M_ROFF_SEEN", "M_PUT_ISSUE", "M_PUT_POSTED", "M_PUT_DONE", "M_SIG_ISSUE", "M_SIG_WAIT", "M_SIG_SEEN", "M_FLUSH_BEG", "M_FLUSH_END",
                                           "P_PICKUP", "P_BATCH", "P_CHUNK", "P_SIGNAL", "P_QUIET_BEG", "P_QUIET_END", "P_DONE", "H_GAP", "H_CNT", "H_PHASE", "H_THR", "H_WD"]
    PHN = ["NONE", "HIP_MALLOC", "VSLOT_RELEASE", "RD_FWD"]; STR = ["ecalc", "futex_wait_queue", "hrtimer_nanosleep"]
    HBN = ["compact_stall", "thr_nivcsw", "pgmajfault", "pgmigrate_fail", "bud_n0_Normal_frag", "mi_MemFree_kb", "mi_Dirty_kb", "mi_Writeback_kb", "kcompactd0_ticks", "kfd_present"]
    EV = {n: i + 1 for i, n in enumerate(EVN)}
    base = 5_000_000_000_000; off = lambda j, t: int(j * 1_500_000 + j * 40e-6 * (t - base))        # node j's clock minus true, ns
    recs = collections.defaultdict(list); prec = collections.defaultdict(list); kcount = collections.defaultdict(int)
    def put(node, ev, t, comm, me, peer, seq, xs, rnd=0, nb=0, aux=0, kp=(5 | (2 << 4))):
        recs[node].append((int(t) + off(node, t), nb, comm, seq, xs, aux, rnd, EV[ev], kp, me, peer))
    t = base + 10_000_000
    for x in range(1, nex + 1):
        slowx = 20 <= x < 26
        begin = [t + int(rng.integers(0, 300_000)) + (4_000_000 if (slowx and j == 2) else 0) for j in range(nodes)]
        pub = [b + 800_000 for b in begin]; done_t = 0
        sig_issue = {}
        for s in range(nodes):
            put(s, "E_BEGIN", begin[s], 4, s, -1, x, x, nb=(nodes - 1) * 8 << 20); put(s, "E_COPY_ISS", begin[s] + 100_000, 4, s, -1, x, x); put(s, "E_COPY_DONE", begin[s] + 400_000, 4, s, -1, x, x)
            put(s, "E_POSTED", begin[s] + 500_000, 4, s, -1, x, x, aux=1); put(s, "R_BEGIN", begin[s] + 520_000, 4, s, -1, x, x, 0, 0, 1); put(s, "R_PUBLISHED", pub[s], 4, s, -1, x, x, 0)
            for r in range(nodes):
                if r == s:
                    continue
                put(s, "M_ROFF_ISSUE", pub[s], 4, s, r, x, x, nb=8 << 20)
        for s in range(nodes):
            for r in range(nodes):
                if r == s:
                    continue
                tw = pub[s] + 100_000; seen = max(tw, pub[r] + 60_000 + int(rng.integers(0, 20_000))); tp = seen + 5_000
                put(s, "M_ROFF_WAIT", tw, 4, s, r, x, x); put(s, "M_ROFF_SEEN", seen, 4, s, r, x, x); put(s, "M_PUT_ISSUE", tp, 4, s, r, x, x, nb=8 << 20)
                k = kcount[s]; kcount[s] += 1
                put(s, "M_PUT_POSTED", tp + 8_000, 4, s, r, x, x, nb=8 << 20, aux=k)
                pick = tp + 20_000 + int(rng.integers(0, 10_000)); first = pick + 3_000; last = first + 300_000; sig = last + 10_000; done = sig + 300_000
                node_p = prec[(s, 0)]; nodeoff = off(s, pick)
                for ev, tt, extra in (("P_PICKUP", pick, 8 << 20), ("P_CHUNK", first, 4 << 20), ("P_CHUNK", last, 4 << 20), ("P_SIGNAL", sig, 8 << 20), ("P_QUIET_BEG", sig + 1_000, 0), ("P_QUIET_END", done, 0), ("P_DONE", done, 0)):
                    node_p.append((int(tt) + nodeoff, extra, 4, x, 0, k, 0, EV[ev], 0, s, r))
                sig_issue[(s, r)] = sig
                put(r, "M_SIG_WAIT", max(pub[r] + 700_000, tp), 4, r, s, x, x)
        for r in range(nodes):
            last = 0
            for s in range(nodes):
                if s == r:
                    continue
                seen = sig_issue[(s, r)] + 40_000 + int(rng.integers(0, 5_000)); put(r, "M_SIG_SEEN", seen, 4, r, s, x, x); last = max(last, seen)
            put(r, "R_SENDS_DONE", last - 300_000, 4, r, -1, x, x, 0); put(r, "R_SIGS_DONE", last, 4, r, -1, x, x, 0); put(r, "R_QUIET_END", last + 20_000, 4, r, -1, x, x, 0); put(r, "R_END", last + 120_000, 4, r, -1, x, x, 0)
            put(r, "E_WAIT", begin[r] + 700_000, 4, r, -1, x, x); put(r, "E_JOINED", last + 130_000, 4, r, -1, x, x); put(r, "E_DONE", last + 140_000, 4, r, -1, x, x); done_t = max(done_t, last + 140_000)
        if x == 20:
            tstall = begin[2]
        t = done_t + 2_000_000
    def hbsnap(node, tt, sid, kind, vals):
        for i, v in enumerate(vals):
            put(node, "H_CNT", tt, 0, -1, -1, kind, 0, rnd=i, nb=v, aux=sid, kp=0)
    for j in range(nodes):                       # the heartbeat: periodic snapshots at the start and the end, and on node 2 a 40 ms gap that ends when its rank 2 begins exchange 20 late
        hbsnap(j, base, 0, 1, [100, 5, 0, 0, 40, 8_000_000, 100, 50, 10, 0]); hbsnap(j, base + 1_000_000_000, 4, 1, [100, 5, 0, 50_000 if j == 2 else 10, 93, 1_000_000, 4096, 2048, 25, 0])
        hbsnap(j, t + 1_000_000, 3, 1, [100 + (30 if j == 2 else 0), 5 + (4 if j == 2 else 0), 0, 50_000 if j == 2 else 10, 93, 1_000_000, 4096, 2048, 25, 0])
    put(2, "H_GAP", tstall - 1_000_000, 0, -1, -1, 0, 0, nb=40_000_000, aux=1, kp=0); hbsnap(2, tstall - 1_000_000, 1, 0, [130, 9, 0])
    put(2, "H_PHASE", tstall - 60_000_000, 101, 0, -1, 0, 0, nb=0, aux=3, kp=0); put(2, "H_PHASE", tstall - 50_000_000, 101, 0, -1, 0, 0, nb=3, aux=2, kp=0); put(2, "H_PHASE", tstall - 55_000_000, 102, 1, -1, 0, 0, nb=0, aux=3, kp=0)
    put(2, "H_WD", tstall - 500_000, 0, -1, -1, 0, 0, nb=30_000_000, aux=7, kp=0)
    for tid, wc, ph, st in ((101, 1, 2, "S"), (102, 2, 3, "R")):
        put(2, "H_THR", tstall - 500_000, 7, 3, 1, tid, wc, rnd=ph, nb=(2_500_000 | (ord(st) << 56)), aux=30, kp=2)
    os.makedirs(d, exist_ok=True)
    clock0 = [{"node": j, "off_m": j * 1_500_000, "off_r": 0, "rtt": 8000 + 100 * j, "rtt_med": 12000, "t0": base} for j in range(1, nodes)]
    clock1 = [{"node": j, "off_m": int(j * 1_500_000 + j * 40e-6 * (t - base)), "off_r": 0, "rtt": 8100, "rtt_med": 12000, "t0": int(t)} for j in range(1, nodes)]
    def wr(pe, node, proxy, apu, rows, comms, clock):
        meta = {"version": 1, "rec_bytes": 40, "pe": pe, "node": node, "nodes": nodes, "ppn": 5, "ppa": 1, "proxy": proxy, "apu": apu, "pxsend": 3, "order": 2, "shift": 1, "round_mb": 1024.0, "sample": 1, "arena_mb": 256,
                "nrec": len(rows), "dropped": 0, "m0": base + off(node, base), "r0": 0, "events": EVN, "hbnames": HBN, "hbstrs": STR, "phases": PHN, "hb_ms": 1, "hb_gap_ms": 20, "comms": comms, "clock": clock, "env": {"COMM_TRACE": "1", "COMM_SHMEM_PX_SEND": "3"}}
        js = json.dumps(meta).encode(); js += b" " * ((8 - len(js) % 8) % 8)
        arr = np.array(rows, dtype=REC) if rows else np.zeros(0, REC)
        with open(os.path.join(d, "trace_pe%d.bin" % pe), "wb") as f:
            f.write(struct.pack("<8sIIQQQQ", b"ECTRACE1", 40, len(js), 64, 64 + len(js), len(arr), 0) + b"\0" * 16); f.write(js); arr.tofile(f)
    for j in range(nodes):
        rows = sorted(recs[j], key=lambda r: r[0])
        wr(5 * j + 4, j, 0, -1, rows, [{"id": 4, "n": nodes, "rank": j, "apu": 0, "node": list(range(nodes))}], [clock0, clock1] if j == 0 else [])
        wr(5 * j, j, 1, 0, sorted(prec[(j, 0)], key=lambda r: r[0]), [], [])


def selftest():
    tmp = tempfile.mkdtemp(prefix="trace_selftest_")
    try:
        synth(os.path.join(tmp, "run"))
        args = argparse.Namespace(tmp=tmp, max_msgs=4000000, top=3, timeline=1, bin_s=0.05, slow_ratio=1.5)
        run = load_run(os.path.join(tmp, "run"), "synthetic"); L = []
        S = run_report(run, args, L)
        txt = "\n".join(L)
        bad = []
        # the clock correction must put the three nodes' exchange begins within 0.3 ms of each other
        e2e = S[("msg", "e2e", 5)]; sig1 = S[("msg", "sig_oneway", 5)]
        if not (300 < e2e["p50"] < 500): bad.append("e2e p50 %.1f us outside 300..500" % e2e["p50"])
        if not (35 < sig1["p50"] < 55): bad.append("signal one way p50 %.1f us, expected ~42" % sig1["p50"])
        if sig1["neg"] > 1: bad.append("negative one-way latencies %.1f%%: the clock correction failed" % sig1["neg"])
        import re
        sec = txt.split("exchange: node that published")[1].split("sender waits")[0]
        m2 = re.search(r"node  2:.*p90\s+([0-9.]+)", sec)
        if not m2 or float(m2.group(1)) < 3000: bad.append("the straggler node 2 was not flagged (p90 lag >= 3000 us)")
        if "Fano" not in txt:   # (the synthetic slowness is skew, which section 7 no longer counts: no persistence line)
            bad.append("no clustering section")
        if "6b slowest" not in txt or "arrival skew" not in txt:
            bad.append("no skew section")
        st = txt.split("8 stalls")[1] if "8 stalls" in txt else ""
        if "n2  np" not in st or "compact_stall+30" not in st or not re.search(r"late exchanges.*", st) or not re.search(r"\|\s+n2\s+np\s+\|\s+40\.0 .*\|\s+[1-9]\d*, [0-9.]+\s*$", st, re.M):
            bad.append("the heartbeat stall of node 2 was not joined with its late exchange")
        s8a = txt.split("8a compaction storms")[1].split("8b stall")[0] if "8a compaction storms" in txt else ""
        if "node 2: 1 storm" not in s8a or "frag 0:93" not in s8a or "kcompactd 15 ticks" not in s8a or "MemFree 1.0 GB" not in s8a or "Dirty+WB 6.0 MB" not in s8a or "absent" not in s8a or "node 1:" in s8a:
            bad.append("the compaction storm of node 2 was not reported (8a)")
        s8 = txt.split("8b stall classification")[1] if "8b stall classification" in txt else ""
        if "t101:VSLOT_RELEASE" not in s8 or "t102:RD_FWD" not in s8 or "futex_wait_queue" not in s8 or "dly 2.5ms" not in s8 or "n2 1/3/1" not in s8:
            bad.append("the phase tags / watchdog snapshot of node 2 were not reported (8b)")
        print(txt if "-v" in sys.argv else "selftest: report %d lines" % len(L))
        for b in bad:
            print("selftest FAIL:", b)
        return 1 if bad else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*", help="DIR or LABEL=DIR")
    ap.add_argument("--out", help="write the report here (default stdout)")
    ap.add_argument("--tmp", default=os.environ.get("TMPDIR"), help="spill directory (default $TMPDIR)")
    ap.add_argument("--top", type=int, default=6, help="slowest exchanges shown / kept (default 6)")
    ap.add_argument("--timeline", type=int, default=1, help="raw event timelines of the N slowest (default 1)")
    ap.add_argument("--bin-s", type=float, default=1.0, help="time bin of the clustering test, seconds (default 1)")
    ap.add_argument("--slow-ratio", type=float, default=2.0, help="slow = duration above this multiple of the median of its class (default 2)")
    ap.add_argument("--max-msgs", type=int, default=6000000, help="message rows kept for the link / time tables (default 6e6; thinned beyond)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(selftest())
    if not args.runs:
        ap.error("no RUN given")
    runs = []
    for a in args.runs:
        lab, _, d = a.partition("=") if "=" in a else (os.path.basename(os.path.normpath(a)), "", a)
        runs.append(load_run(d, lab))
    L = []; sums = []
    for r in runs:
        sums.append(run_report(r, args, L))
    if len(runs) > 1:
        compare(runs, sums, L)
    txt = "\n".join(L) + "\n"
    if args.out:
        with open(args.out, "w") as f:
            f.write(txt)
    else:
        sys.stdout.write(txt)


if __name__ == "__main__":
    main()
