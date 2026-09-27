/* unpack_digits - Phase 15 IO (W2): the packed digit files of ecalc (ECALC_OUT_PACKED=1, ecalc/packed_fmt.h) to the exact
 * ASCII file ("2." + digits + "\n", byte-identical to the run's normal output), or compared with an ASCII reference without
 * writing anything.  Streaming: a reader thread reads the limbs, the main thread formats them (OpenMP), a consumer thread
 * writes the ASCII or compares it with the reference as it reads that; memory about 1.7 GB at the default chunk.
 *
 *   unpack_digits [options] <file | part files ...>
 *     (no -o, no --cmp)  the ASCII to stdout (e.g. | sha1sum)
 *     -o <file>          the ASCII to <file>
 *     --cmp <ref>        compare with the ASCII file <ref> (cmp's answer: identical, or the first differing byte and the
 *                        line; a length difference is reported as EOF on the shorter one); nothing is written
 *     -n                 no output and no compare: the residue check alone
 *     --no-res           skip the residue check
 *     -c <MB>            the input chunk (default 256 MB of limbs)
 *     -q                 no summary line
 *
 * The parts of a multi-node run (<outfile>.part0000 ...) may be given in any order: they are sorted by the part index in
 * their headers, and their limb ranges must join (part 0 the top limbs, the last part limb 0).  The residue check: every
 * part's digits [k0, k1), formatted here, as a decimal number mod the eight T1 primes against the residues its run stored
 * in the header (the run's own "digits == X mod q" values) -- it checks this program's formatting and the file's bytes.
 *
 * Exit: 0 ok (and identical with --cmp); 1 a difference or a residue mismatch; 2 usage, I/O or format error.
 * Build: make -C tools (gcc -O3 -fopenmp -pthread). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <omp.h>
#include "../ecalc/packed_fmt.h"

typedef unsigned __int128 u128;
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + 1e-6 * t.tv_usec; }
static void die(const char *fmt, const char *a) { fprintf(stderr, "unpack_digits: "); fprintf(stderr, fmt, a); fprintf(stderr, "\n"); exit(2); }

static inline void fmt18(char *o, uint64_t v)
{
    uint64_t hi = v / 1000000000ULL, lo = v % 1000000000ULL;
    for (int i = 17; i >= 9; i--) { o[i] = (char)('0' + lo % 10); lo /= 10; }
    for (int i = 8; i >= 0; i--) { o[i] = (char)('0' + hi % 10); hi /= 10; }
}
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)(((u128)a * b) % q); }
static uint64_t powmod(uint64_t b, uint64_t e, uint64_t q) { uint64_t r = 1 % q; b %= q; while (e) { if (e & 1) r = mulmod(r, b, q); b = mulmod(b, b, q); e >>= 1; } return r; }
/* a digit string mod nq primes (18 digits per step; OpenMP over pieces joined with 10^len) */
static void digits_mods(const char *s, size_t n, const uint64_t *q, int nq, uint64_t *out)
{
    for (int j = 0; j < nq; j++) out[j] = 0;
    if (!n) return;
    int T = omp_get_max_threads(); if ((size_t)T > n / 4096 + 1) T = (int)(n / 4096 + 1);
    uint64_t *cv = malloc((size_t)T * nq * 8);
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; t++) {
        size_t a = n * t / T, e = n * (t + 1) / T, k = a; uint64_t v[ECP_NQ], p18[ECP_NQ];
        for (int j = 0; j < nq; j++) { v[j] = 0; p18[j] = 1000000000000000000ULL % q[j]; }
        for (; k + 18 <= e; k += 18) { uint64_t c = 0; for (int i = 0; i < 18; i++) c = c * 10 + (uint64_t)(s[k + i] - '0'); for (int j = 0; j < nq; j++) v[j] = (uint64_t)(((u128)v[j] * p18[j] + c) % q[j]); }
        for (; k < e; k++) for (int j = 0; j < nq; j++) v[j] = (uint64_t)(((u128)v[j] * 10 + (uint64_t)(s[k] - '0')) % q[j]);
        for (int j = 0; j < nq; j++) cv[(size_t)t * nq + j] = v[j];
    }
    for (int j = 0; j < nq; j++) { uint64_t r = 0; for (int t = 0; t < T; t++) { size_t a = n * t / T, e = n * (t + 1) / T; r = (mulmod(r, powmod(10, e - a, q[j]), q[j]) + cv[(size_t)t * nq + j]) % q[j]; } out[j] = r; }
    free(cv);
}

struct part { const char *name; ecp_hdr h; };
static int part_cmp(const void *a, const void *b) { const struct part *x = a, *y = b; return (int)x->h.part - (int)y->h.part; }

/* the input: chunks of limbs over the parts in order */
struct inbuf { uint64_t *l; size_t m; int part; size_t pos; int last; };   /* pos: the chunk's first limb within its part (file order) */
static struct { struct part *p; int np; size_t CHL; struct inbuf b[2]; sem_t full[2], empty[2]; int err; double t_io; size_t bytes; } R;
static void *reader(void *a)
{
    (void)a; int k = 0;
    for (int i = 0; i < R.np; i++) {
        const ecp_hdr *h = &R.p[i].h; size_t n = h->hi - h->lo;
        int fd = open(R.p[i].name, O_RDONLY); if (fd < 0) { R.err = errno; fprintf(stderr, "unpack_digits: %s: %s\n", R.p[i].name, strerror(errno)); }
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        for (size_t pos = 0; pos < n || (n == 0 && pos == 0); ) {
            sem_wait(&R.empty[k]);
            struct inbuf *b = &R.b[k]; size_t m = n - pos < R.CHL ? n - pos : R.CHL;
            double t0 = now(); size_t want = m * 8, got = 0; char *d = (char *)b->l;
            while (fd >= 0 && got < want) { ssize_t r = pread(fd, d + got, want - got, (off_t)(ECP_HDR_BYTES + pos * 8 + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) { R.err = r < 0 ? errno : EIO; fprintf(stderr, "unpack_digits: %s: short read at limb %zu\n", R.p[i].name, pos + got / 8); break; } got += (size_t)r; }
            if (fd >= 0) posix_fadvise(fd, (off_t)(ECP_HDR_BYTES + pos * 8), (off_t)want, POSIX_FADV_DONTNEED);
            R.t_io += now() - t0; R.bytes += got;
            b->m = R.err ? 0 : m; b->part = i; b->pos = pos; pos += m; b->last = (i == R.np - 1 && pos >= n) || R.err;
            sem_post(&R.full[k]); k ^= 1;
            if (R.err) { if (fd >= 0) close(fd); return 0; }
            if (n == 0) break;
        }
        if (fd >= 0) close(fd);
    }
    return 0;
}
/* the output: ASCII pieces to a file / stdout, or compared with a reference */
struct outbuf { char *mem; const char *s; size_t n; int last; };
static struct { struct outbuf b[2]; sem_t full[2], empty[2]; int fd, cmp_fd, mode; size_t off, line; char *cbuf; int differ; double t_io; } W;   /* mode 0 write, 1 compare, 2 nothing */
static void *consumer(void *a)
{
    (void)a;
    for (int k = 0;; k ^= 1) {
        sem_wait(&W.full[k]); struct outbuf *b = &W.b[k];
        double t0 = now();
        if (W.mode == 0 && b->n) {
            size_t done = 0; while (done < b->n) { ssize_t w = write(W.fd, b->s + done, b->n - done); if (w < 0 && errno == EINTR) continue; if (w <= 0) { fprintf(stderr, "unpack_digits: write: %s\n", strerror(errno)); exit(2); } done += (size_t)w; }
        } else if (W.mode == 1 && !W.differ) {
            size_t done = 0;
            while (done < b->n && !W.differ) {
                ssize_t r = read(W.cmp_fd, W.cbuf, b->n - done < (64u << 20) ? b->n - done : (64u << 20));
                if (r < 0 && errno == EINTR) continue;
                if (r < 0) { fprintf(stderr, "unpack_digits: reading the reference: %s\n", strerror(errno)); exit(2); }
                if (r == 0) { printf("unpack_digits: EOF on the reference after byte %zu (the packed digits are longer)\n", W.off + done); W.differ = 1; break; }
                if (memcmp(W.cbuf, b->s + done, (size_t)r)) {
                    size_t i = 0; while (W.cbuf[i] == b->s[done + i]) i++;
                    size_t line = W.line; for (size_t j = 0; j < done + i; j++) if (b->s[j] == '\n') line++;
                    printf("unpack_digits: differ: byte %zu, line %zu (reference '%c', packed '%c')\n", W.off + done + i + 1, line + 1, W.cbuf[i], b->s[done + i]);
                    W.differ = 1; break;
                }
                done += (size_t)r;
            }
            for (size_t j = 0; j < b->n; j++) if (b->s[j] == '\n') W.line++;
            if (b->last && !W.differ) { char c; ssize_t r = read(W.cmp_fd, &c, 1); if (r > 0) { printf("unpack_digits: EOF on the packed digits after byte %zu (the reference is longer)\n", W.off + b->n); W.differ = 1; } }
        }
        W.off += b->n; W.t_io += now() - t0;
        int last = b->last; sem_post(&W.empty[k]);
        if (last) return 0;
    }
}

int main(int argc, char **argv)
{
    const char *outname = 0, *cmpname = 0; int nores = 0, quiet = 0, none = 0; size_t chunk_mb = 256;
    struct part *p = calloc((size_t)argc, sizeof *p); int np = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outname = argv[++i];
        else if (!strcmp(argv[i], "--cmp") && i + 1 < argc) cmpname = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) chunk_mb = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--no-res")) nores = 1;
        else if (!strcmp(argv[i], "-n")) none = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "usage: unpack_digits [-o out | --cmp ref | -n] [--no-res] [-c MB] [-q] <packed file | parts ...>\n"); return 2; }
        else p[np++].name = argv[i];
    }
    if (!np) { fprintf(stderr, "usage: unpack_digits [-o out | --cmp ref | -n] [--no-res] [-c MB] [-q] <packed file | parts ...>\n"); return 2; }
    if (chunk_mb < 1) chunk_mb = 1;
    /* the headers */
    for (int i = 0; i < np; i++) {
        FILE *f = fopen(p[i].name, "rb"); if (!f) die("cannot open %s", p[i].name);
        if (fread(&p[i].h, sizeof p[i].h, 1, f) != 1 || memcmp(p[i].h.magic, ECP_MAGIC, 8)) die("%s is not a packed digit file (no ECPACK18 header)", p[i].name);
        fclose(f);
        const ecp_hdr *h = &p[i].h;
        if (h->endian != ECP_ENDIAN) die("%s was written on a host of the other byte order", p[i].name);
        if (h->version != ECP_VERSION || h->hdr_bytes != ECP_HDR_BYTES || h->limb_bytes != 8 || h->limb_digits != 18 || h->order != 1) die("%s: an unknown format version or layout", p[i].name);
        if (!h->complete) die("%s: incomplete (the run did not close it: complete = 0)", p[i].name);
    }
    qsort(p, (size_t)np, sizeof *p, part_cmp);
    for (int i = 0; i < np; i++) {
        const ecp_hdr *h = &p[i].h, *h0 = &p[0].h;
        if ((int)h->part != i || (int)h->nparts != np) { fprintf(stderr, "unpack_digits: %s is part %u of %u; %d files given (parts 0..%d needed)\n", p[i].name, h->part, h->nparts, np, np - 1); return 2; }
        if (h->d != h0->d || h->d_out != h0->d_out || h->nl != h0->nl || h->pad != h0->pad) die("%s belongs to another run (d, d_out or the limb count differ)", p[i].name);
        if (i == 0 && h->hi != h->nl) die("%s: part 0 does not end at the top limb", p[i].name);
        if (i > 0 && h->hi != p[i - 1].h.lo) die("%s: its limb range does not join the part before it", p[i].name);
        if (i == np - 1 && h->lo != 0) die("%s: the last part does not reach limb 0", p[i].name);
    }
    const ecp_hdr *H = &p[0].h; size_t pad = H->pad, nl = H->nl, d_out = H->d_out;
    /* the pipeline */
    R.p = p; R.np = np; R.CHL = (chunk_mb << 20) / 8;
    for (int k = 0; k < 2; k++) { R.b[k].l = malloc(R.CHL * 8); W.b[k].mem = malloc(R.CHL * 18 + 64); if (!R.b[k].l || !W.b[k].mem) die("%s", "out of memory"); sem_init(&R.full[k], 0, 0); sem_init(&R.empty[k], 0, 1); sem_init(&W.full[k], 0, 0); sem_init(&W.empty[k], 0, 1); }
    W.mode = none ? 2 : cmpname ? 1 : 0; W.fd = 1;
    if (W.mode == 0 && outname && strcmp(outname, "-")) { W.fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (W.fd < 0) die("cannot create %s", outname); }
    if (W.mode == 1) { W.cmp_fd = open(cmpname, O_RDONLY); if (W.cmp_fd < 0) die("cannot open %s", cmpname); posix_fadvise(W.cmp_fd, 0, 0, POSIX_FADV_SEQUENTIAL); W.cbuf = malloc(64u << 20); }
    double t0 = now(), t_fmt = 0, t_res = 0;
    pthread_t rth, wth; pthread_create(&rth, 0, reader, 0); pthread_create(&wth, 0, consumer, 0);
    uint64_t res[ECP_NQ], acc[ECP_NQ]; int cur = -1, badres = 0; size_t ndig_out = 0;
    for (int k = 0, ko = 0;; k ^= 1, ko ^= 1) {
        sem_wait(&R.full[k]); struct inbuf *ib = &R.b[k];
        if (R.err) { fprintf(stderr, "unpack_digits: read error\n"); return 2; }
        const ecp_hdr *h = &p[ib->part].h;
        if (ib->part != cur) { cur = ib->part; memset(acc, 0, sizeof acc); }
        sem_wait(&W.empty[ko]); struct outbuf *ob = &W.b[ko];
        double ta = now();
        char *tmp = ob->mem + 2; size_t m = ib->m;
#pragma omp parallel for schedule(static)
        for (size_t j = 0; j < m; j++) fmt18(tmp + 18 * j, ib->l[j]);
        size_t c0 = 18 * (nl - h->hi + ib->pos), c1 = c0 + 18 * m;             /* the chunk's characters; digit = char - pad */
        size_t x = c0 > pad ? c0 - pad : 0, y = c1 > pad ? c1 - pad : 0; if (!m) x = y = 0;
        double tb = now(); t_fmt += tb - ta;
        if (!nores) {                                                          /* the part's digits [k0, k1) (d_out does not cut them) */
            size_t rx = x > h->k0 ? x : h->k0, ry = y < h->k1 ? y : h->k1;
            if (ry > rx) { digits_mods(tmp + (rx + pad - c0), ry - rx, h->q, ECP_NQ, res); for (int j = 0; j < ECP_NQ; j++) acc[j] = (mulmod(acc[j], powmod(10, ry - rx, h->q[j]), h->q[j]) + res[j]) % h->q[j]; }
        }
        t_res += now() - tb;
        size_t e = h->k1 < d_out + 1 ? h->k1 : d_out + 1, ox = x > h->k0 ? x : h->k0, oy = y < e ? y : e;   /* the digits that go out */
        char *s = tmp + (ox + pad - c0); size_t n = oy > ox ? oy - ox : 0;
        if (n && ox == 0) { s[-1] = s[0]; s[0] = '.'; s--; n++; }               /* "2." */
        if (oy == d_out + 1 && n) s[n++] = '\n';                               /* after digit d_out (the tail digits in the buffer are overwritten) */
        ndig_out += oy > ox ? oy - ox : 0;
        int last = ib->last;
        int part_end = last || R.b[k].pos + m >= h->hi - h->lo;
        if (part_end && !nores) {
            int bad = 0; for (int j = 0; j < ECP_NQ; j++) if (acc[j] != h->dres[j]) bad++;
            if (bad) { fprintf(stderr, "unpack_digits: %s: the digits differ from the run's residues at %d of %d primes\n", p[cur].name, bad, ECP_NQ); badres = 1; }
        }
        ob->s = s; ob->n = W.mode == 2 ? 0 : n; ob->last = last;
        sem_post(&R.empty[k]); sem_post(&W.full[ko]);
        if (last) break;
    }
    pthread_join(rth, 0); pthread_join(wth, 0);
    if (W.mode == 0 && W.fd != 1) { if (fsync(W.fd) != 0 && errno != EINVAL) fprintf(stderr, "unpack_digits: fsync: %s\n", strerror(errno)); posix_fadvise(W.fd, 0, 0, POSIX_FADV_DONTNEED); close(W.fd); }
    double t = now() - t0;
    if (!quiet) fprintf(stderr, "unpack_digits: %d part%s, %zu digits (d_out %zu): read %.2f GB in %.1f s (%.2f GB/s in the reader), %s %.2f GB (%.1f s in the %s); format %.1f s, residues %s%.1f s; %.1f s wall, %.2f GB/s of ASCII%s\n",
                        np, np > 1 ? "s" : "", ndig_out, d_out, R.bytes * 1e-9, R.t_io, R.t_io > 0 ? R.bytes * 1e-9 / R.t_io : 0,
                        W.mode == 0 ? "wrote" : W.mode == 1 ? "compared" : "made", W.off * 1e-9, W.t_io, W.mode == 1 ? "comparer" : "writer", t_fmt, nores ? "(off) " : badres ? "MISMATCH " : "ok ", t_res, t, t > 0 ? W.off * 1e-9 / t : 0,
                        W.mode == 1 ? (W.differ ? "; DIFFER" : "; IDENTICAL") : "");
    if (W.mode == 1 && !W.differ) printf("unpack_digits: identical to %s (%zu bytes)\n", cmpname, W.off);
    return badres || W.differ ? 1 : 0;
}
