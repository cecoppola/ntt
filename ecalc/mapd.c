/* mapd.c -- ECALC_MAPD=1: helper processes (one per APU, NUMA-pinned) run hipMemCreate for the plane pools and the dbig VMM arena
 * chunks and export each as a POSIX fd handle (SCM_RIGHTS over a Unix socket pair); the main process imports
 * (hipMemImportFromShareableHandle), maps and sets access (dbig.c vmm_map_run; mapd_pool_alloc for the pools).
 * The helper is this very executable started with argv[1] = --mapd-helper (posix_spawn: no HIP state is inherited).  It serves a
 * FIFO of requests (kind, count, size): pool chunks first (rns_init), then the arena (kind 1, `final`: the helper exits after
 * it); a helper whose device never gets an arena is closed at the end of rns_init (mapd_end_init).  The imported memory stays
 * valid.  Any failure (spawn, helper init, create/export, 60 s without a chunk, import) kills that helper, logs one line, and
 * the remaining chunks of that APU are created in-process (never a wrong result, never a hang). */
#include <hip/hip_runtime.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_inject.h"
#include "mapd.h"
extern char **environ;


#define MAPD_NQ 4
#define MAPD_TIMEOUT_MS 60000
#define MAPD_FD0 200                                   /* the child's end of the socket pair is dup'ed to MAPD_FD0 + dev */
static const hipMemAllocationHandleType FDT = hipMemHandleTypePosixFileDescriptor;
struct req { uint32_t magic, kind, n, final; uint64_t chunk; };   /* kind 0 = plane pool, 1 = arena; final: the helper exits after this request */
#define MAGIC 0x4d415044u

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static void nap_ms(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, 0); }

/* ---- helper side ---- */
static int numa_of(int dev)
{
    char bus[32], path[128]; int v = -1;
    if (hipDeviceGetPCIBusId(bus, sizeof bus, dev) == hipSuccess) {
        for (char *c = bus; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A';
        snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/numa_node", bus);
        FILE *f = fopen(path, "r"); if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    }
    return v;
}
static void pin_node(int node)                          /* the node's CPUs within the affinity inherited from the job step (slurm's mask) */
{
    char path[96], buf[4096]; if (node < 0) return;
    snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(path, "r"); if (!f) return; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    cpu_set_t s, in; CPU_ZERO(&s);
    for (char *p = strtok(buf, ",\n"); p; p = strtok(NULL, ",\n")) { int a, b; if (sscanf(p, "%d-%d", &a, &b) == 2) { for (int i = a; i <= b && i < CPU_SETSIZE; i++) CPU_SET(i, &s); } else if (sscanf(p, "%d", &a) == 1) CPU_SET(a, &s); }
    if (sched_getaffinity(0, sizeof in, &in) == 0) CPU_AND(&s, &s, &in);
    if (CPU_COUNT(&s)) sched_setaffinity(0, sizeof s, &s);
}
static int send_fd(int sock, int fd, char tag)
{
    struct iovec io; io.iov_base = &tag; io.iov_len = 1;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u; memset(&u, 0, sizeof u);
    struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &io; m.msg_iovlen = 1;
    if (fd >= 0) {
        m.msg_control = u.buf; m.msg_controllen = sizeof u.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(sock, &m, MSG_NOSIGNAL) == 1 ? 0 : -1;
}
int mapd_is_helper(int argc, char **argv) { return argc >= 3 && !strcmp(argv[1], "--mapd-helper"); }
int mapd_helper_main(int argc, char **argv)             /* argv: --mapd-helper <dev> <parent pid> */
{
    int dev = atoi(argv[2]), sock = MAPD_FD0 + dev; pid_t pp = argc >= 4 ? (pid_t)atoi(argv[3]) : 0;
    prctl(PR_SET_PDEATHSIG, SIGKILL); if (pp > 0 && getppid() != pp) return 1;   /* (the parent died before the prctl; with ecalc as PID 1 the original pid is 1 and the check holds) */
    signal(SIGPIPE, SIG_IGN);
    int die_dev = -1, die_k = -1; char die_kind = 'a'; char tdb[48]; const char *td = ec_test_str("mapd_die", tdb, sizeof tdb);   /* test knob "<dev>:<k>[:p]": that helper exits (status 7) after sending k arena chunks (":p": k pool chunks) */
    if (td) { char kc = 0; int nf = sscanf(td, "%d:%d:%c", &die_dev, &die_k, &kc); if (nf == 3 && kc == 'p') die_kind = 'p'; }
    if (hipSetDevice(dev) != hipSuccess || hipFree(0) != hipSuccess) { (void)send_fd(sock, -1, 'F'); return 2; }
    pin_node(numa_of(dev));
    if (send_fd(sock, -1, 'R')) return 5;                  /* the helper's runtime is up (the main process syncs on this before it measures device memory) */
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice;
    prop.location.id = dev; prop.requestedHandleType = FDT;
    for (;;) {
        struct req rq; size_t g = 0;
        while (g < sizeof rq) { ssize_t r = read(sock, (char *)&rq + g, sizeof rq - g); if (r <= 0) return 0; g += (size_t)r; }   /* EOF: the main process is done with us, gone, or gave up */
        if (rq.magic != MAGIC) return 3;
        for (uint32_t i = 0; i < rq.n; i++) {
            if (dev == die_dev && rq.kind == (die_kind == 'p' ? 0u : 1u) && (int)i == die_k) _exit(7);
            hipMemGenericAllocationHandle_t h; int fd = -1;
            if (hipMemCreate(&h, (size_t)rq.chunk, &prop, 0) != hipSuccess || hipMemExportToShareableHandle(&fd, h, FDT, 0) != hipSuccess) { (void)send_fd(sock, -1, 'E'); return 4; }
            int rc = send_fd(sock, fd, 'x'); close(fd);
            if (rc) return 5;
        }
        if (rq.final) return 0;                           /* exit: the imported chunks stay valid */
    }
}

/* ---- main side ---- */
static struct md { pid_t pid; int sock; int state; int synced, fin; int left; int n; double wait, imp; } g_md[MAPD_NQ];   /* state: 0 none, 1 helper alive (requests may follow), 3 failed / finished */
static int g_on;

static void reap(struct md *m, int d, int kill_now)     /* bounded: the helper may sit in an uninterruptible driver call; after 2 s it is abandoned */
{
    if (m->pid <= 0) return;
    if (kill_now) kill(m->pid, SIGKILL);
    for (int i = 0; i < 600; i++) {
        int st; pid_t r = waitpid(m->pid, &st, WNOHANG);
        if (r == m->pid || (r < 0 && errno != EINTR)) { m->pid = 0; return; }
        if (i == 100 && !kill_now) kill(m->pid, SIGKILL);   /* asked to exit 0.5 s ago */
        if (i >= 400) break;
        nap_ms(5);
    }
    fprintf(stderr, "ecalc: ECALC_MAPD: helper of APU %d (pid %d) did not exit within 2 s: abandoned\n", d, (int)m->pid); m->pid = 0;
}
static void reap_try(struct md *m) { if (m->pid > 0) { int st; pid_t r = waitpid(m->pid, &st, WNOHANG); if (r == m->pid || (r < 0 && errno != EINTR)) m->pid = 0; } }
static void md_fail(int d, const char *why)
{
    struct md *m = &g_md[d];
    fprintf(stderr, "ecalc: ECALC_MAPD: helper of APU %d failed (%s) after %d chunks: the remaining %d chunks are created in-process\n", d, why, m->n, m->left);
    if (m->sock >= 0) { close(m->sock); m->sock = -1; }
    reap(m, d, 1);
    m->state = 3; m->left = 0;
}
void mapd_early(void)
{
    const char *e = getenv("ECALC_MAPD"); if (!e || atoi(e) == 0) return;
    const char *vm = getenv("DB_POOL_VMM"); if (vm && atoi(vm) == 0) return;
    const char *pp = getenv("ECALC_PE_PER_GPU"); if (pp && atoi(pp) != 0) { fprintf(stderr, "ecalc: ECALC_MAPD: not used with a process per APU (phase 1)\n"); return; }
    const char *vis = getenv("ROCR_VISIBLE_DEVICES"); if (!vis || !*vis) vis = getenv("HIP_VISIBLE_DEVICES");
    int nd = MAPD_NQ; if (vis && *vis) { nd = 1; for (const char *c = vis; *c; c++) if (*c == ',') nd++; }
    if (nd > MAPD_NQ) nd = MAPD_NQ;
    char exe[1024]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { fprintf(stderr, "ecalc: ECALC_MAPD: no /proc/self/exe: switch ignored\n"); return; }
    exe[n] = 0;
    for (int d = 0; d < nd; d++) {
        int sv[2]; struct md *m = &g_md[d]; m->sock = -1;
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { m->state = 3; continue; }
        posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa); posix_spawn_file_actions_adddup2(&fa, sv[1], MAPD_FD0 + d);
        char a2[16], a3[16]; snprintf(a2, sizeof a2, "%d", d); snprintf(a3, sizeof a3, "%d", (int)getpid()); char *argv[] = { exe, (char *)"--mapd-helper", a2, a3, NULL }; pid_t pid;
        int rc = posix_spawn(&pid, exe, &fa, NULL, argv, environ); posix_spawn_file_actions_destroy(&fa); close(sv[1]);
        if (rc) { close(sv[0]); m->state = 3; fprintf(stderr, "ecalc: ECALC_MAPD: cannot start the helper of APU %d (%s): in-process mapping\n", d, strerror(rc)); continue; }
        m->pid = pid; m->sock = sv[0]; m->state = 1;
    }
    g_on = 1;
}
int mapd_active(void) { return g_on; }
void mapd_request(int dev, int kind, int nchunks, size_t chunk, int final)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ || nchunks <= 0) return;
    struct md *m = &g_md[dev]; if (m->state != 1) return;
    if (kind == 1 && m->left != 0) { md_fail(dev, "pool chunks requested earlier were not taken"); return; }   /* the FIFO order would shift: stop using the helper */
    struct req rq = { MAGIC, (uint32_t)kind, (uint32_t)nchunks, (uint32_t)final, (uint64_t)chunk };
    if (send(m->sock, &rq, sizeof rq, MSG_NOSIGNAL) != (ssize_t)sizeof rq) { md_fail(dev, "request"); return; }   /* (MSG_NOSIGNAL: a dead helper is an error here, not SIGPIPE) */
    m->left += nchunks; m->fin = final;
}
static int md_recv(int dev, int *fd, char *tag)           /* one message (poll, then recvmsg); 0 = failed (md_fail called) */
{
    struct md *m = &g_md[dev]; struct pollfd pf = { m->sock, POLLIN, 0 };
    int pr; do { pr = poll(&pf, 1, MAPD_TIMEOUT_MS); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) { md_fail(dev, pr == 0 ? "timeout" : "poll"); return 0; }
    struct iovec io; io.iov_base = tag; io.iov_len = 1; *tag = 0; *fd = -1;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u; memset(&u, 0, sizeof u);
    struct msghdr mh; memset(&mh, 0, sizeof mh); mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = u.buf; mh.msg_controllen = sizeof u.buf;
    ssize_t r; do { r = recvmsg(m->sock, &mh, MSG_CMSG_CLOEXEC); } while (r < 0 && errno == EINTR);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (r == 1 && c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof(int));
    if (r != 1 || *tag == 'E' || *tag == 'F') { md_fail(dev, r == 0 ? "helper exited" : *tag == 'E' ? "create or export in the helper" : *tag == 'F' ? "helper HIP init" : "no message received"); return 0; }
    return 1;
}
void mapd_sync(int dev)                                  /* rns_init, before it measures device memory: wait (60 s at most) until the helper's runtime is up */
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ) return;
    struct md *m = &g_md[dev]; if (m->state != 1 || m->synced) return;
    double t0 = now(); int fd; char tag;
    if (md_recv(dev, &fd, &tag)) { if (fd >= 0) close(fd); m->synced = 1; m->wait += now() - t0; }
}
void mapd_ready(int dev)                                 /* wait (outside any lock) until the next chunk of `dev` can be read: a stuck helper then does not hold dbig's mapper lock for the 60 s */
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ) return;
    struct md *m = &g_md[dev]; if (m->state != 1 || m->left <= 0) return;
    struct pollfd pf = { m->sock, POLLIN, 0 }; int pr; do { pr = poll(&pf, 1, MAPD_TIMEOUT_MS); } while (pr < 0 && errno == EINTR);
    if (pr == 0) md_fail(dev, "timeout");
}
int mapd_chunk(int dev, void **handle)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ) return 0;
    struct md *m = &g_md[dev]; if (m->state != 1 || m->left <= 0) return 0;
    double t0 = now(); int fd; char tag;
    for (;;) { if (!md_recv(dev, &fd, &tag)) return 0; if (tag == 'R' && fd < 0) { m->synced = 1; continue; } break; }
    if (fd < 0) { md_fail(dev, "no handle received"); return 0; }
    double t1 = now(); m->wait += t1 - t0;
    int cur = 0; (void)hipGetDevice(&cur); (void)hipSetDevice(dev);
    hipMemGenericAllocationHandle_t h = 0;
    hipError_t he = hipMemImportFromShareableHandle(&h, (void *)(intptr_t)fd, FDT); close(fd);
    (void)hipSetDevice(cur);
    if (he != hipSuccess) { md_fail(dev, "import"); return 0; }
    m->imp += now() - t1; m->n++;
    if (--m->left == 0 && m->fin) { m->state = 3; reap_try(m); }   /* (a helper exits by itself after its final request; reaped here if already gone, else in mapd_stat / mapd_finish) */
    *handle = (void *)h;
    return 1;
}
int mapd_stat(int dev, int *n, double *wait, double *imp)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ || !g_md[dev].n) return 0;
    reap_try(&g_md[dev]);
    *n = g_md[dev].n; *wait = g_md[dev].wait; *imp = g_md[dev].imp; return 1;
}
void mapd_finish(int dev)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ) return;
    struct md *m = &g_md[dev];
    if (m->sock >= 0) { close(m->sock); m->sock = -1; }
    reap(m, dev, 0);
    m->state = 3;
}
void mapd_end_init(void)                                 /* end of rns_init: a helper that has no work coming (no arena requested for its device) is finished and reaped now */
{
    if (!g_on) return;
    for (int d = 0; d < MAPD_NQ; d++) { struct md *m = &g_md[d]; if (m->state == 1 && m->left == 0 && m->pid > 0) { if (m->sock >= 0) { close(m->sock); m->sock = -1; } } }
    for (int d = 0; d < MAPD_NQ; d++) { struct md *m = &g_md[d]; if (m->state == 1 && m->left == 0) { reap(m, d, 0); m->state = 3; } }
}

/* ---- plane pools (phase 2): a contiguous VA range over helper-created chunks ---- */
static struct prec { char *base; size_t cap; int dev, n; hipMemGenericAllocationHandle_t *h; size_t *sz; } g_pr[32];
static pthread_mutex_t g_pr_mx = PTHREAD_MUTEX_INITIALIZER;
void mapd_pool_request(int dev, size_t bytes, size_t C)  /* the helper of `dev` will create the chunks of one pool of `bytes` (whole 2 MiB): bytes / C chunks of C, then the remainder */
{
    if (!mapd_active() || C == 0) return;
    if (bytes >= C) mapd_request(dev, 0, (int)(bytes / C), C, 0);
    if (bytes % C) mapd_request(dev, 0, 1, bytes % C, 0);
}
void *mapd_pool_alloc(int dev, size_t bytes, size_t C)   /* NULL: use hipMalloc.  The chunks are the helper's (mapd_pool_request, same bytes and C), or created here where the helper failed */
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ || g_md[dev].state != 1 || C == 0) return NULL;
    int cur; if (hipGetDevice(&cur) != hipSuccess) return NULL;
    int n = (int)(bytes / C) + (bytes % C ? 1 : 0), nd = 0, ok = 0, mapped = 0; struct prec r; memset(&r, 0, sizeof r);
    r.dev = dev; r.cap = bytes; r.n = n; r.h = (hipMemGenericAllocationHandle_t *)calloc((size_t)n, sizeof *r.h); r.sz = (size_t *)calloc((size_t)n, sizeof *r.sz);
    if (!r.h || !r.sz) {
        fprintf(stderr, "ecalc: ECALC_MAPD: plane pool of %.2f GB on APU %d: out of host memory: hipMalloc\n", bytes / 1e9, dev);
        free(r.h); free(r.sz); if (g_md[dev].state == 1) md_fail(dev, "pool chunks");
        return NULL;
    }
    (void)hipSetDevice(dev);
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = dev;
    size_t gran = 0; if (hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !gran) gran = (size_t)2 << 20;
    if (hipGetDeviceCount(&nd) != hipSuccess || nd > MAPD_NQ) nd = nd > MAPD_NQ ? MAPD_NQ : 0;
    if (nd > 0 && hipMemAddressReserve((void **)&r.base, bytes, gran, 0, 0) == hipSuccess) {
        ok = 1;
        for (int i = 0; i < n && ok; i++) {
            size_t sz = (i < n - 1 || !(bytes % C)) ? C : bytes % C; void *hh = 0; r.sz[i] = sz;
            if (mapd_chunk(dev, &hh)) r.h[i] = (hipMemGenericAllocationHandle_t)hh;
            else if (hipMemCreate(&r.h[i], sz, &prop, 0) != hipSuccess) { r.h[i] = 0; ok = 0; break; }
            if (hipMemMap(r.base + (size_t)i * C, sz, 0, r.h[i], 0) != hipSuccess) { ok = 0; (void)hipMemRelease(r.h[i]); r.h[i] = 0; break; }
            mapped = i + 1;
        }
        if (ok) {
            hipMemAccessDesc ad[MAPD_NQ]; memset(ad, 0, sizeof ad);
            for (int c = 0; c < nd; c++) { ad[c].location.type = hipMemLocationTypeDevice; ad[c].location.id = c; ad[c].flags = hipMemAccessFlagsProtReadWrite; }
            if (hipMemSetAccess(r.base, bytes, ad, nd) != hipSuccess) ok = 0;
        }
    }
    (void)hipSetDevice(cur);
    if (!ok) {
        fprintf(stderr, "ecalc: ECALC_MAPD: plane pool of %.2f GB on APU %d could not be built from chunks: hipMalloc\n", bytes / 1e9, dev);
        if (r.base) { for (int i = 0; i < mapped; i++) { (void)hipMemUnmap(r.base + (size_t)i * C, r.sz[i]); (void)hipMemRelease(r.h[i]); r.h[i] = 0; } for (int i = mapped; i < n; i++) if (r.h[i]) (void)hipMemRelease(r.h[i]); (void)hipMemAddressFree(r.base, bytes); }
        free(r.h); free(r.sz); if (g_md[dev].state == 1) md_fail(dev, "pool chunks");
        return NULL;
    }
    pthread_mutex_lock(&g_pr_mx);
    for (int k = 0; k < 32; k++) if (!g_pr[k].base) { g_pr[k] = r; pthread_mutex_unlock(&g_pr_mx); fprintf(stderr, "ecalc: ECALC_MAPD: plane pool of %.2f GB on APU %d built from %d chunks of the helper\n", bytes / 1e9, dev, n); return r.base; }
    pthread_mutex_unlock(&g_pr_mx);
    for (int i = 0; i < n; i++) { (void)hipMemUnmap(r.base + (size_t)i * C, r.sz[i]); (void)hipMemRelease(r.h[i]); } (void)hipMemAddressFree(r.base, bytes); free(r.h); free(r.sz);   /* (no free slot: cannot track) */
    return NULL;
}
int mapd_pool_free(int dev, void *p)                     /* 1 if p was a pool of mapd_pool_alloc (now released) */
{
    if (!p) return 0;
    struct prec r; int found = 0; pthread_mutex_lock(&g_pr_mx);
    for (int k = 0; k < 32; k++) if (g_pr[k].base == (char *)p) { r = g_pr[k]; memset(&g_pr[k], 0, sizeof g_pr[k]); found = 1; break; }
    pthread_mutex_unlock(&g_pr_mx);
    if (!found) return 0;
    int cur = -1; if (hipGetDevice(&cur) != hipSuccess) cur = -1;
    (void)hipSetDevice(dev); (void)hipDeviceSynchronize();
    size_t off = 0; for (int i = 0; i < r.n; i++) { (void)hipMemUnmap(r.base + off, r.sz[i]); (void)hipMemRelease(r.h[i]); off += r.sz[i]; }
    (void)hipMemAddressFree(r.base, r.cap); free(r.h); free(r.sz);
    if (cur >= 0) (void)hipSetDevice(cur);
    return 1;
}
