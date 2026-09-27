/* wbench - Phase 15 IO (W1, TARGET §6 item 5): the part-file writer's I/O pattern without a GPU run, to measure a file
 * system's write (and read-back) rate the way ecalc's writer thread writes: chunks of C MB split over T pwrite threads,
 * in one of ecalc's modes (the data is prebuilt; only the writer's I/O is timed).  Run on one node, then on many at once
 * (srun -N64 ... with a per-node file name: "%h" in the name is the host name, "%r" SLURM_PROCID).
 *
 *   wbench [-m direct|buffered|sync|drop] [-t threads (8)] [-c chunk MB (256)] [-s size GB (8)] [-r] [-k] <file>
 *     -r  read the file back afterwards (O_DIRECT for direct, else buffered + DONTNEED), and report its rate
 *     -k  keep the file (default: removed)
 * One line: mode, threads, chunk, GB, seconds, GB/s (write, then read), MemAvailable / Cached before and after (the page
 * cache is HBM on the APU).  The data is digit-like ASCII.  Build: make -C tools. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <omp.h>

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + 1e-6 * t.tv_usec; }
static void meminfo(double *avail, double *cached)
{
    FILE *f = fopen("/proc/meminfo", "r"); char k[64]; unsigned long long v; *avail = *cached = 0; if (!f) return;
    char line[256]; while (fgets(line, sizeof line, f)) if (sscanf(line, "%63s %llu", k, &v) == 2) { if (!strcmp(k, "MemAvailable:")) *avail = v * 1024e-9; if (!strcmp(k, "Cached:")) *cached = v * 1024e-9; }
    fclose(f);
}
static int pio(int fd, char *p, size_t n, off_t off, int wr)
{
    while (n) { ssize_t r = wr ? pwrite(fd, p, n, off) : pread(fd, p, n, off); if (r < 0 && errno == EINTR) continue; if (r <= 0) return r < 0 ? errno : EIO; p += r; n -= (size_t)r; off += r; }
    return 0;
}
int main(int argc, char **argv)
{
    const char *mode = "direct", *name = 0; int T = 8, rd = 0, keep = 0; size_t cmb = 256; double gb = 8;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) T = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cmb = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) gb = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rd = 1;
        else if (!strcmp(argv[i], "-k")) keep = 1;
        else if (argv[i][0] != '-') name = argv[i];
        else { fprintf(stderr, "usage: wbench [-m direct|buffered|sync|drop] [-t threads] [-c chunk MB] [-s GB] [-r] [-k] <file (%%h host, %%r rank)>\n"); return 2; }
    }
    if (!name || T < 1 || !cmb) { fprintf(stderr, "wbench: a file name is needed\n"); return 2; }
    char host[128] = "?", fn[4096]; gethostname(host, sizeof host); const char *pr = getenv("SLURM_PROCID");
    { size_t o = 0; for (const char *p = name; *p && o < sizeof fn - 130; p++) { if (p[0] == '%' && p[1] == 'h') { o += (size_t)snprintf(fn + o, sizeof fn - o, "%s", host); p++; } else if (p[0] == '%' && p[1] == 'r') { o += (size_t)snprintf(fn + o, sizeof fn - o, "%s", pr ? pr : "0"); p++; } else fn[o++] = *p; } fn[o] = 0; }
    int direct = !strcmp(mode, "direct"), drop = !strcmp(mode, "drop"), sync_ = !strcmp(mode, "sync");
    if (!direct && !drop && !sync_ && strcmp(mode, "buffered")) { fprintf(stderr, "wbench: mode %s?\n", mode); return 2; }
    size_t C = cmb << 20, total = (size_t)(gb * 1e9) / 4096 * 4096, nch = (total + C - 1) / C;
    char *buf[2]; for (int b = 0; b < 2; b++) { if (posix_memalign((void **)&buf[b], 1 << 21, C)) return 2; for (size_t i = 0; i < C; i++) buf[b][i] = (char)('0' + (i * 7 + b) % 10); }
    int fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0), 0644);
    if (fd < 0) { fprintf(stderr, "wbench: %s: %s\n", fn, strerror(errno)); return 2; }
    double a0, c0, a1, c1; meminfo(&a0, &c0);
    int err = 0; double t0 = now();
    for (size_t k = 0; k < nch && !err; k++) {                /* (the data is ready: the writer's own time is what is measured, as ecalc's writer thread) */
        size_t len = total - k * C < C ? total - k * C : C; off_t off = (off_t)(k * C); char *p = buf[k & 1];
        size_t seg = ((len + T - 1) / T + 4095) & ~(size_t)4095;
#pragma omp parallel for num_threads(T) schedule(static) reduction(|:err)
        for (int t = 0; t < T; t++) { size_t s = (size_t)t * seg; if (s < len) err |= pio(fd, p + s, len - s < seg ? len - s : seg, off + (off_t)s, 1); }
        if (drop && !err) { if (fdatasync(fd)) err = errno; posix_fadvise(fd, off, (off_t)len, POSIX_FADV_DONTNEED); }
    }
    if (!err && (direct || sync_ || drop) && fsync(fd)) err = errno;
    double tw = now() - t0;
    if (!direct && (sync_ || drop)) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd); meminfo(&a1, &c1);
    double tr = 0;
    if (rd && !err) {
        int rfd = open(fn, O_RDONLY | (direct ? O_DIRECT : 0));
        if (rfd >= 0) {
            double r0 = now();
            for (size_t k = 0; k < nch && !err; k++) {
                size_t len = total - k * C < C ? total - k * C : C, seg = ((len + T - 1) / T + 4095) & ~(size_t)4095; off_t off = (off_t)(k * C);
#pragma omp parallel for num_threads(T) schedule(static) reduction(|:err)
                for (int t = 0; t < T; t++) { size_t s = (size_t)t * seg; if (s < len) err |= pio(rfd, buf[0] + s, len - s < seg ? len - s : seg, off + (off_t)s, 0); }
            }
            tr = now() - r0; posix_fadvise(rfd, 0, 0, POSIX_FADV_DONTNEED); close(rfd);
        }
    }
    if (!keep) unlink(fn);
    char rs[96] = ""; if (rd) snprintf(rs, sizeof rs, "; read %.2f s = %.3f GB/s", tr, tr > 0 ? total * 1e-9 / tr : 0.0);
    printf("wbench %s %s: mode %s, %d threads, %zu MB chunks, %.2f GB: write %.2f s = %.3f GB/s%s; MemAvailable %.1f -> %.1f GB, Cached %.1f -> %.1f GB%s%s\n",
           host, fn, mode, T, cmb, total * 1e-9, tw, total * 1e-9 / tw, rs, a0, a1, c0, c1, err ? "; ERROR " : "", err ? strerror(err) : "");
    return err ? 1 : 0;
}
