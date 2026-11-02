/* t_mapbench - S1/E1 standalone timing: where does the start-up mapping of the per-APU arenas go, and do helper processes that create
 * the allocations and export them as POSIX file-descriptor handles (SCM_RIGHTS over a Unix socket) map faster than one process with four threads?
 * Not linked into ecalc.  Build: make tests/t_mapbench (hipcc).  One run = one arm, one repetition:
 *     t_mapbench <arm> [chunks=20] [gib=2] [load=0|1] [check=0|1]
 * arms (per chunk: create, map, set access for all APUs, first-touch kernel writing one word per 4 KiB page):
 *   A  one process, 4 threads, one global lock around create+map+access+touch of a chunk (the background mapper's shape)
 *   B  as A without the lock
 *   C  as B, each thread pinned to the CPUs of its APU's NUMA node
 *   D  4 helper processes (one per APU, pinned) hipMemCreate with a POSIX-fd handle type and export; the main process's 4 threads
 *      (not pinned) import, map, set access and touch
 *   E  as D with the main threads pinned too
 * Output: one "RESULT" line of key=value pairs, seconds, mean over the 4 threads of each thread's summed phase time, plus the wall
 * (go to the last touch) and the helper-side numbers.  check=1 adds the functional checks (peer read, unmap/remap, helper exit, release). */
#include <hip/hip_runtime.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
extern char **environ;

#define NQ 4
#define OK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, #x, hipGetErrorString(e_)); exit(3); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

static int g_chunks = 20; static size_t g_chunk = (size_t)2 << 30; static int g_load, g_check; static char g_arm = 'A';
static const hipMemAllocationHandleType FDT = hipMemHandleTypePosixFileDescriptor;

/* ---- NUMA node of an APU (PCI sysfs) and pinning ---- */
static int numa_of(int dev)
{
    char bus[32], path[128]; int v = -1;
    if (hipDeviceGetPCIBusId(bus, sizeof bus, dev) == hipSuccess) {
        for (char *c = bus; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A';
        snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/numa_node", bus);
        FILE *f = fopen(path, "r"); if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    }
    return v >= 0 ? v : dev;
}
static void pin_node(int node)
{
    char path[96], buf[4096]; snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(path, "r"); if (!f) return; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    cpu_set_t s; CPU_ZERO(&s);
    for (char *p = strtok(buf, ",\n"); p; p = strtok(NULL, ",\n")) { int a, b; if (sscanf(p, "%d-%d", &a, &b) == 2) { for (int i = a; i <= b && i < CPU_SETSIZE; i++) CPU_SET(i, &s); } else if (sscanf(p, "%d", &a) == 1) CPU_SET(a, &s); }
    if (CPU_COUNT(&s)) sched_setaffinity(0, sizeof s, &s);
}

/* ---- kernels: one word per 4 KiB page ---- */
__global__ void k_touch(unsigned long *p, size_t pages, unsigned long val) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; if (i < pages) p[i * 512] = val + i; }
__global__ void k_verify(const unsigned long *p, size_t pages, unsigned long val, unsigned long long *bad) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; if (i < pages && p[i * 512] != val + i) atomicAdd(bad, 1ULL); }
static void touch(int dev, void *p, size_t bytes, unsigned long val)
{
    size_t pages = bytes / 4096; OK(hipSetDevice(dev));
    k_touch<<<(unsigned)((pages + 255) / 256), 256>>>((unsigned long *)p, pages, val); OK(hipGetLastError()); OK(hipDeviceSynchronize());
}
static unsigned long long verify(int on_dev, void *p, size_t bytes, unsigned long val)   /* run on APU on_dev; returns the mismatches */
{
    size_t pages = bytes / 4096; OK(hipSetDevice(on_dev));
    unsigned long long *bad, h = 0; OK(hipMalloc(&bad, 8)); OK(hipMemset(bad, 0, 8));
    k_verify<<<(unsigned)((pages + 255) / 256), 256>>>((const unsigned long *)p, pages, val, bad); OK(hipGetLastError()); OK(hipDeviceSynchronize());
    OK(hipMemcpy(&h, bad, 8, hipMemcpyDeviceToHost)); OK(hipFree(bad)); return h;
}

/* ---- CPU load: memory-bandwidth threads (the seed thread's state) ---- */
static volatile int g_load_stop;
static void *load_thr(void *a)
{
    size_t n = (size_t)128 << 20; char *x = (char *)malloc(n), *y = (char *)malloc(n); memset(x, 1, n); memset(y, 2, n); (void)a;
    while (!g_load_stop) memcpy(y, x, n);
    free(x); free(y); return NULL;
}

/* ---- fd passing ---- */
static void send_fd(int sock, int fd)
{
    char b = 'x'; struct iovec io; io.iov_base = &b; io.iov_len = 1;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u; memset(&u, 0, sizeof u);
    struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = u.buf; m.msg_controllen = sizeof u.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &fd, sizeof(int));
    if (sendmsg(sock, &m, 0) != 1) { perror("sendmsg"); exit(4); }
}
static int recv_fd(int sock)
{
    char b; struct iovec io; io.iov_base = &b; io.iov_len = 1;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u; memset(&u, 0, sizeof u);
    struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = u.buf; m.msg_controllen = sizeof u.buf;
    if (recvmsg(sock, &m, 0) != 1) { perror("recvmsg"); exit(4); }
    struct cmsghdr *c = CMSG_FIRSTHDR(&m); if (!c || c->cmsg_type != SCM_RIGHTS) { fprintf(stderr, "no fd received\n"); exit(4); }
    int fd; memcpy(&fd, CMSG_DATA(c), sizeof(int)); return fd;
}
static void rd(int s, void *p, size_t n) { size_t g = 0; while (g < n) { ssize_t r = read(s, (char *)p + g, n - g); if (r <= 0) { perror("read"); exit(4); } g += (size_t)r; } }
static void wr(int s, const void *p, size_t n) { if (write(s, p, n) != (ssize_t)n) { perror("write"); exit(4); } }

/* ---- helper process: create + export ---- */
struct hstat { double init, cr, ex, t_end; };
static int helper_main(int dev, int sock, int chunks, size_t chunk)
{
    double t0 = now(); pin_node(numa_of(dev) >= 0 ? numa_of(dev) : dev);
    OK(hipSetDevice(dev)); OK(hipFree(0));
    struct hstat hs; memset(&hs, 0, sizeof hs); hs.init = now() - t0;
    char c = 'R'; wr(sock, &c, 1); rd(sock, &c, 1);               /* ready; wait for go */
    double tg = now();
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice;
    prop.location.id = dev; prop.requestedHandleType = FDT;
    hipMemGenericAllocationHandle_t *hh = (hipMemGenericAllocationHandle_t *)calloc((size_t)chunks, sizeof *hh);
    for (int i = 0; i < chunks; i++) {
        double a = now(); OK(hipMemCreate(&hh[i], chunk, &prop, 0)); double b = now();
        int fd = -1; OK(hipMemExportToShareableHandle(&fd, hh[i], FDT, 0)); double e = now();
        send_fd(sock, fd); close(fd);
        hs.cr += b - a; hs.ex += e - b;
    }
    hs.t_end = now() - tg; wr(sock, &hs, sizeof hs);
    rd(sock, &c, 1);                                               /* 'Q': exit (handles freed with the process unless imported) */
    return 0;
}

/* ---- main process ---- */
struct th { int dev, sock; void *base; hipMemGenericAllocationHandle_t *h; double cr, wait, imp, map, acc, touch, end; hipStream_t unused; };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_barrier_t g_bar; static double g_t0;
static hipMemAccessDesc g_ad[NQ];

static void *worker(void *arg)
{
    th *t = (th *)arg; int d = t->dev;
    if (g_arm == 'C' || g_arm == 'E') pin_node(numa_of(d));
    OK(hipSetDevice(d));
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = d;
    if (g_arm == 'D' || g_arm == 'E') {} else prop.requestedHandleType = hipMemHandleTypeNone;
    pthread_barrier_wait(&g_bar);                                  /* all threads (and the go to the helpers) start together */
    for (int k = 0; k < g_chunks; k++) {
        char *slot = (char *)t->base + (size_t)k * g_chunk;
        if (g_arm == 'A') pthread_mutex_lock(&g_lock);
        double a = now();
        if (g_arm == 'D' || g_arm == 'E') {
            int fd = recv_fd(t->sock); double b = now(); t->wait += b - a; a = b;
            OK(hipMemImportFromShareableHandle(&t->h[k], (void *)(intptr_t)fd, FDT)); close(fd);
            b = now(); t->imp += b - a;
        } else {
            OK(hipMemCreate(&t->h[k], g_chunk, &prop, 0)); double b = now(); t->cr += b - a;
        }
        double b = now();
        OK(hipMemMap(slot, g_chunk, 0, t->h[k], 0)); double c = now(); t->map += c - b;
        OK(hipMemSetAccess(slot, g_chunk, g_ad, NQ)); double e = now(); t->acc += e - c;
        touch(d, slot, g_chunk, (unsigned long)d << 40 | (unsigned long)k << 32);
        double f = now(); t->touch += f - e;
        if (g_arm == 'A') pthread_mutex_unlock(&g_lock);
    }
    t->end = now() - g_t0;
    return NULL;
}

static int spawn_helper(int dev, int sock_child, int sock_parent)
{
    char a1[16], a2[16], a3[16], a4[32], exe[512]; snprintf(a1, sizeof a1, "%d", dev); snprintf(a2, sizeof a2, "%d", sock_child);
    snprintf(a3, sizeof a3, "%d", g_chunks); snprintf(a4, sizeof a4, "%zu", g_chunk);
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); if (n < 0) { perror("readlink"); exit(4); } exe[n] = 0;
    char *argv[] = { exe, (char *)"--helper", a1, a2, a3, a4, NULL }; pid_t pid;
    (void)sock_parent;
    if (posix_spawn(&pid, exe, NULL, NULL, argv, environ)) { perror("posix_spawn"); exit(4); }
    return (int)pid;
}

int main(int argc, char **argv)
{
    if (argc >= 7 && !strcmp(argv[1], "--helper")) return helper_main(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), (size_t)strtoull(argv[5], 0, 10));
    if (argc < 2) { fprintf(stderr, "usage: t_mapbench A|B|C|D|E [chunks=20] [gib=2] [load=0|1] [check=0|1]\n"); return 2; }
    g_arm = argv[1][0]; double gib = 2;
    for (int i = 2; i < argc; i++) { int v; double f; if (sscanf(argv[i], "chunks=%d", &v) == 1) g_chunks = v; else if (sscanf(argv[i], "gib=%lf", &f) == 1) gib = f; else if (sscanf(argv[i], "load=%d", &v) == 1) g_load = v; else if (sscanf(argv[i], "check=%d", &v) == 1) g_check = v; }
    g_chunk = (size_t)(gib * (double)(1 << 30));
    int helpers = (g_arm == 'D' || g_arm == 'E');
    int socks[NQ][2]; pid_t pids[NQ]; double h_init = 0;
    double ts = now();
    if (helpers) for (int d = 0; d < NQ; d++) {                    /* before any HIP call in this process */
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks[d])) { perror("socketpair"); return 4; }
        pids[d] = spawn_helper(d, socks[d][1], socks[d][0]);
    }
    int nd = 0; OK(hipGetDeviceCount(&nd)); if (nd < NQ) { fprintf(stderr, "need %d APUs, have %d\n", NQ, nd); return 2; }
    th T[NQ]; memset(T, 0, sizeof T); pthread_t tid[NQ];
    memset(g_ad, 0, sizeof g_ad);
    for (int c = 0; c < NQ; c++) { g_ad[c].location.type = hipMemLocationTypeDevice; g_ad[c].location.id = c; g_ad[c].flags = hipMemAccessFlagsProtReadWrite; }
    for (int d = 0; d < NQ; d++) {                                 /* HIP up and the address ranges reserved before the clock starts (ecalc has both at that point) */
        OK(hipSetDevice(d)); OK(hipFree(0));
        T[d].dev = d; T[d].sock = socks[d][0]; T[d].h = (hipMemGenericAllocationHandle_t *)calloc((size_t)g_chunks + 2, sizeof(hipMemGenericAllocationHandle_t));
        OK(hipMemAddressReserve(&T[d].base, (size_t)(g_chunks + 2) * g_chunk, 0, 0, 0));
    }
    if (helpers) {
        for (int d = 0; d < NQ; d++) { char c; rd(socks[d][0], &c, 1); }
        h_init = now() - ts;
    }
    pthread_t lt[64]; int nl = 0;
    if (g_load) { nl = 48; for (int i = 0; i < nl; i++) pthread_create(&lt[i], 0, load_thr, 0); usleep(700000); }
    size_t f0, tot; OK(hipSetDevice(0)); OK(hipMemGetInfo(&f0, &tot));
    pthread_barrier_init(&g_bar, 0, NQ + 1);
    for (int d = 0; d < NQ; d++) pthread_create(&tid[d], 0, worker, &T[d]);
    g_t0 = now(); if (helpers) for (int d = 0; d < NQ; d++) wr(socks[d][0], "G", 1);
    pthread_barrier_wait(&g_bar);
    for (int d = 0; d < NQ; d++) pthread_join(tid[d], 0);
    double wall = now() - g_t0; double wmax = 0; for (int d = 0; d < NQ; d++) if (T[d].end > wmax) wmax = T[d].end;
    struct hstat hs[NQ]; memset(hs, 0, sizeof hs);
    if (helpers) for (int d = 0; d < NQ; d++) rd(socks[d][0], &hs[d], sizeof hs[d]);
    size_t f1; OK(hipSetDevice(0)); OK(hipMemGetInfo(&f1, &tot));
    g_load_stop = 1; for (int i = 0; i < nl; i++) pthread_join(lt[i], 0);
    double cr = 0, wt = 0, im = 0, mp = 0, ac = 0, tc = 0, hcr = 0, hex = 0, hend = 0;
    for (int d = 0; d < NQ; d++) { cr += T[d].cr; wt += T[d].wait; im += T[d].imp; mp += T[d].map; ac += T[d].acc; tc += T[d].touch; hcr += hs[d].cr; hex += hs[d].ex; if (hs[d].t_end > hend) hend = hs[d].t_end; }
    cr /= NQ; wt /= NQ; im /= NQ; mp /= NQ; ac /= NQ; tc /= NQ; hcr /= NQ; hex /= NQ;
    printf("RESULT arm=%c load=%d chunks=%d gib=%.2f wall=%.3f create=%.3f wait=%.3f import=%.3f map=%.3f access=%.3f touch=%.3f helper_create=%.3f helper_export=%.3f helper_end=%.3f helper_init=%.3f used_gib=%.1f\n",
           g_arm, g_load, g_chunks, gib, wall, cr, wt, im, mp, ac, tc, hcr, hex, hend, h_init, (double)(f0 - f1) / (1 << 30));
    (void)wmax;

    if (g_check) {
        unsigned long long bad = 0; int rc = 0;
        /* 1. data written by APU d (first touch) read by APU d+1 through the main mapping (peer access after import) */
        for (int d = 0; d < NQ; d++) for (int k = 0; k < g_chunks; k += (g_chunks > 4 ? g_chunks / 4 : 1))
            bad += verify((d + 1) % NQ, (char *)T[d].base + (size_t)k * g_chunk, g_chunk, (unsigned long)d << 40 | (unsigned long)k << 32);
        printf("CHECK peer_read mismatches=%llu\n", bad); rc |= bad != 0;
        /* 2. unmap + remap of chunk 0 at the spare slot, then back (ecalc remaps near the memory edge) */
        bad = 0; int ok2 = 1;
        for (int d = 0; d < NQ; d++) {
            OK(hipSetDevice(d));
            char *s0 = (char *)T[d].base, *s1 = (char *)T[d].base + (size_t)g_chunks * g_chunk;
            OK(hipMemUnmap(s0, g_chunk)); OK(hipMemMap(s1, g_chunk, 0, T[d].h[0], 0)); OK(hipMemSetAccess(s1, g_chunk, g_ad, NQ));
            bad += verify((d + 1) % NQ, s1, g_chunk, (unsigned long)d << 40);
            OK(hipMemUnmap(s1, g_chunk)); OK(hipMemMap(s0, g_chunk, 0, T[d].h[0], 0)); OK(hipMemSetAccess(s0, g_chunk, g_ad, NQ));
            bad += verify(d, s0, g_chunk, (unsigned long)d << 40);
        }
        printf("CHECK unmap_remap mismatches=%llu\n", bad); ok2 = bad == 0; rc |= !ok2;
        /* 3. helpers exit, the imported memory stays valid */
        if (helpers) {
            for (int d = 0; d < NQ; d++) { wr(socks[d][0], "Q", 1); int st; waitpid(pids[d], &st, 0); }
            usleep(300000); bad = 0;
            size_t g0; OK(hipSetDevice(0)); OK(hipMemGetInfo(&g0, &tot));
            for (int d = 0; d < NQ; d++) for (int k = 0; k < g_chunks; k += (g_chunks > 4 ? g_chunks / 4 : 1))
                bad += verify((d + 2) % NQ, (char *)T[d].base + (size_t)(k) * g_chunk, g_chunk, (unsigned long)d << 40 | (unsigned long)k << 32);
            printf("CHECK after_helper_exit mismatches=%llu used_gib=%.1f (was %.1f)\n", bad, (double)(g0 < f0 ? f0 - g0 : 0) / (1 << 30), (double)(f0 - f1) / (1 << 30)); rc |= bad != 0;
        }
        /* 4. release returns the memory */
        for (int d = 0; d < NQ; d++) {
            OK(hipSetDevice(d));
            OK(hipMemUnmap(T[d].base, (size_t)g_chunks * g_chunk));
            for (int k = 0; k < g_chunks; k++) OK(hipMemRelease(T[d].h[k]));
        }
        size_t f2; OK(hipSetDevice(0)); usleep(300000); OK(hipMemGetInfo(&f2, &tot));
        printf("CHECK release free_before=%.1f free_after=%.1f GiB (APU0; the other APUs' pages count against their own NUMA)\n", (double)f1 / (1 << 30), (double)f2 / (1 << 30));
        printf("CHECKS %s\n", rc ? "FAILED" : "PASSED");
    } else if (helpers) for (int d = 0; d < NQ; d++) { wr(socks[d][0], "Q", 1); int st; waitpid(pids[d], &st, 0); }
    return 0;
}
