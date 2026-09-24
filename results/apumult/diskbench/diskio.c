/* diskio.c -- sequential file write/read benchmark with memory sampling (agent M, apumult spill study).
 * gcc -O2 -pthread diskio.c -o diskio
 * ./diskio -w|-r -f FILE -s GiB [-b MiB] [-t threads] [-d] [-y] [-a] [-k GiB]
 *   -w/-r   write / read          -d  O_DIRECT (buffers are 4 KiB aligned)
 *   -t T    T threads, each with its own block buffer; block i goes to thread i%T (queue depth = T)
 *   -y      fsync after the write (included in the timed total)
 *   -a      posix_fadvise(DONTNEED) on the whole file at the end (after fsync); for -r also before reading
 *   -k G    buffered write: every G GiB, sync_file_range(WAIT) + fadvise DONTNEED the finished range (bounded page cache)
 * Prints interval rates every 2 s with MemAvailable / Cached / Dirty from /proc/meminfo, then totals. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <getopt.h>

static int wr = 1, direct = 0, T = 1, dofsync = 0, dontneed = 0; static size_t bs = 1ull << 30, nblk; static double kgib = 0;
static int fd; static atomic_ullong done_bytes; static atomic_int finished;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static void meminfo(double *avail, double *cached, double *dirty) {
    FILE *f = fopen("/proc/meminfo", "r"); char l[256]; unsigned long long v;
    *avail = *cached = *dirty = 0;
    while (fgets(l, sizeof l, f)) {
        if (sscanf(l, "MemAvailable: %llu", &v) == 1) *avail = v / 1048576.0;
        else if (sscanf(l, "Cached: %llu", &v) == 1) *cached = v / 1048576.0;
        else if (sscanf(l, "Dirty: %llu", &v) == 1) *dirty = v / 1048576.0;
    }
    fclose(f);
}
static void pmem(const char *tag) { double a, c, d; meminfo(&a, &c, &d); printf("MEM %-14s MemAvailable %7.1f GiB  Cached %7.1f GiB  Dirty %6.1f GiB\n", tag, a, c, d); fflush(stdout); }
static void *worker(void *arg) {
    int t = (int)(long)arg; char *buf; if (posix_memalign((void **)&buf, 4096, bs)) abort();
    for (size_t i = 0; i < bs; i += 8) *(unsigned long long *)(buf + i) = 0x9E3779B97F4A7C15ull * (i + t + 1);
    for (size_t i = t; i < nblk; i += T) {
        size_t off = i * bs, got = 0;
        while (got < bs) {
            ssize_t r = wr ? pwrite(fd, buf + got, bs - got, off + got) : pread(fd, buf + got, bs - got, off + got);
            if (r <= 0) { perror(wr ? "pwrite" : "pread"); exit(1); }
            got += r;
        }
        atomic_fetch_add(&done_bytes, bs);
    }
    free(buf); return NULL;
}
int main(int argc, char **argv) {
    const char *fn = NULL; double gib = 1; int c;
    while ((c = getopt(argc, argv, "wrf:s:b:t:dyak:")) != -1) switch (c) {
        case 'w': wr = 1; break; case 'r': wr = 0; break; case 'f': fn = optarg; break; case 's': gib = atof(optarg); break;
        case 'b': bs = (size_t)(atof(optarg) * 1048576); break; case 't': T = atoi(optarg); break; case 'd': direct = 1; break;
        case 'y': dofsync = 1; break; case 'a': dontneed = 1; break; case 'k': kgib = atof(optarg); break; default: return 2; }
    nblk = (size_t)(gib * 1073741824.0 / bs);
    fd = open(fn, (wr ? O_WRONLY | O_CREAT : O_RDONLY) | (direct ? O_DIRECT : 0), 0644); if (fd < 0) { perror("open"); return 1; }
    printf("%s %s %.0f GiB bs %.0f MiB threads %d%s%s%s\n", wr ? "WRITE" : "READ", direct ? "O_DIRECT" : "buffered", nblk * bs / 1073741824.0, bs / 1048576.0, T,
           dofsync ? " +fsync" : "", dontneed ? " +DONTNEED" : "", kgib > 0 ? " +periodic-drop" : "");
    if (!wr && dontneed) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    pmem("before");
    pthread_t th[256]; double t0 = now();
    for (int t = 0; t < T; t++) pthread_create(&th[t], NULL, worker, (void *)(long)t);
    double tl = t0; unsigned long long bl = 0, kdone = 0; double peak_cached = 0, min_avail = 1e30;
    for (;;) {
        usleep(200000);
        unsigned long long b = atomic_load(&done_bytes); double tn = now();
        if (kgib > 0 && wr && !direct) {                         /* bounded page cache: flush + drop completed prefix (blocks < b/bs-T may be in flight; use a safe prefix) */
            size_t safe = b / bs > (size_t)T ? (b / bs - T) * bs : 0;
            if (safe >= kdone + (size_t)(kgib * 1073741824.0)) { sync_file_range(fd, kdone, safe - kdone, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER); posix_fadvise(fd, kdone, safe - kdone, POSIX_FADV_DONTNEED); kdone = safe; }
        }
        if (tn - tl >= 2.0 || b == nblk * bs) {
            double a, cc, d; meminfo(&a, &cc, &d); if (cc > peak_cached) peak_cached = cc; if (a < min_avail) min_avail = a;
            printf("  t %6.1f s  %7.1f GiB  interval %6.2f GB/s  MemAvail %6.1f  Cached %6.1f  Dirty %5.1f GiB\n", tn - t0, b / 1073741824.0, (b - bl) / (tn - tl) / 1e9, a, cc, d); fflush(stdout);
            tl = tn; bl = b;
        }
        if (b == nblk * bs) break;
    }
    for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
    double t1 = now(); pmem("after-io");
    if (wr && dofsync) fsync(fd);
    double t2 = now(); if (wr && dofsync) pmem("after-fsync");
    if (dontneed) { posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); pmem("after-DONTNEED"); }
    close(fd);
    double tot = nblk * bs;
    printf("RESULT %s %s %.0f GiB T=%d: io %.2f s = %.2f GB/s; incl fsync %.2f s = %.2f GB/s; peak Cached %.1f GiB, min MemAvail %.1f GiB\n",
           wr ? "write" : "read", direct ? "O_DIRECT" : "buffered", tot / 1073741824.0, T, t1 - t0, tot / (t1 - t0) / 1e9, t2 - t0, tot / (t2 - t0) / 1e9, peak_cached, min_avail);
    return 0;
}
