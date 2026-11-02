/* mapd.c -- ECALC_MAPD=1: helper processes (one per APU, NUMA-pinned) run hipMemCreate for the dbig VMM arena chunks and export each as a POSIX
 * fd handle (SCM_RIGHTS over a Unix socket pair); the main process imports (hipMemImportFromShareableHandle), maps and sets access as before
 * (dbig.c vmm_map_run, its mapper lock unchanged).  Experiment: tests/t_mapbench.c, results/MAPBENCH.md.
 * The helper is this very executable started with argv[1] = --mapd-helper (posix_spawn = fork + exec: no HIP state is inherited).  A helper
 * exits when its chunks are sent; the imported memory stays valid (E1).  Any failure (spawn, helper init, create/export, 60 s without a chunk,
 * import) kills that helper, logs one line, and the remaining chunks of that APU are created in-process (never a wrong result, never a hang). */
#include <hip/hip_runtime.h>
#include <errno.h>
#include <poll.h>
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
#include "mapd.h"
extern char **environ;

#define MAPD_NQ 4
#define MAPD_TIMEOUT_MS 60000
#define MAPD_FD0 200                                   /* the child's end of the socket pair is dup'ed to MAPD_FD0 + dev */
static const hipMemAllocationHandleType FDT = hipMemHandleTypePosixFileDescriptor;
struct req { uint32_t magic, n; uint64_t chunk; };
#define MAGIC 0x4d415044u

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

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
static void pin_node(int node)
{
    char path[96], buf[4096]; if (node < 0) return;
    snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(path, "r"); if (!f) return; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    cpu_set_t s; CPU_ZERO(&s);
    for (char *p = strtok(buf, ",\n"); p; p = strtok(NULL, ",\n")) { int a, b; if (sscanf(p, "%d-%d", &a, &b) == 2) { for (int i = a; i <= b && i < CPU_SETSIZE; i++) CPU_SET(i, &s); } else if (sscanf(p, "%d", &a) == 1) CPU_SET(a, &s); }
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
int mapd_helper_main(int argc, char **argv)
{
    int dev = atoi(argv[2]), sock = MAPD_FD0 + dev; (void)argc;
    prctl(PR_SET_PDEATHSIG, SIGKILL); if (getppid() == 1) return 1;
    signal(SIGPIPE, SIG_IGN);
    int die_dev = -1, die_k = -1; const char *td = getenv("ECALC_MAPD_TEST_DIE");   /* test knob: "<dev>:<k>" -- that helper exits (status 7) after sending k chunks */
    if (td) sscanf(td, "%d:%d", &die_dev, &die_k);
    if (hipSetDevice(dev) != hipSuccess || hipFree(0) != hipSuccess) { (void)send_fd(sock, -1, 'F'); return 2; }
    pin_node(numa_of(dev));
    struct req rq; size_t g = 0;
    while (g < sizeof rq) { ssize_t r = read(sock, (char *)&rq + g, sizeof rq - g); if (r <= 0) return 0; g += (size_t)r; }   /* EOF: the main process is gone or gave up */
    if (rq.magic != MAGIC) return 3;
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice;
    prop.location.id = dev; prop.requestedHandleType = FDT;
    hipMemGenericAllocationHandle_t *hh = (hipMemGenericAllocationHandle_t *)calloc(rq.n, sizeof *hh);
    for (uint32_t i = 0; i < rq.n; i++) {
        if (dev == die_dev && (int)i == die_k) _exit(7);
        int fd = -1;
        if (hipMemCreate(&hh[i], (size_t)rq.chunk, &prop, 0) != hipSuccess || hipMemExportToShareableHandle(&fd, hh[i], FDT, 0) != hipSuccess) { (void)send_fd(sock, -1, 'E'); return 4; }
        int rc = send_fd(sock, fd, 'x'); close(fd);
        if (rc) return 5;
    }
    return 0;                                          /* exit: the imported chunks stay valid (results/MAPBENCH.md E1) */
}

/* ---- main side ---- */
static struct md { pid_t pid; int sock; int state; int left; size_t chunk; int n; double wait, imp; } g_md[MAPD_NQ];   /* state: 0 none, 1 spawned, 2 requested, 3 failed / finished */
static int g_on;

static void md_fail(int d, const char *why)
{
    struct md *m = &g_md[d];
    fprintf(stderr, "ecalc: ECALC_MAPD: helper of APU %d failed (%s) after %d chunks: the remaining %d chunks are created in-process\n", d, why, m->n, m->left);
    if (m->pid > 0) { kill(m->pid, SIGKILL); waitpid(m->pid, 0, 0); m->pid = 0; }
    if (m->sock >= 0) { close(m->sock); m->sock = -1; }
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
        char a2[16]; snprintf(a2, sizeof a2, "%d", d); char *argv[] = { exe, (char *)"--mapd-helper", a2, NULL }; pid_t pid;
        int rc = posix_spawn(&pid, exe, &fa, NULL, argv, environ); posix_spawn_file_actions_destroy(&fa); close(sv[1]);
        if (rc) { close(sv[0]); m->state = 3; fprintf(stderr, "ecalc: ECALC_MAPD: cannot start the helper of APU %d (%s): in-process mapping\n", d, strerror(rc)); continue; }
        m->pid = pid; m->sock = sv[0]; m->state = 1;
    }
    g_on = 1;
}
void mapd_request(int dev, int nchunks, size_t chunk)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ) return;
    struct md *m = &g_md[dev]; if (m->state != 1) return;
    struct req rq = { MAGIC, (uint32_t)nchunks, (uint64_t)chunk };
    if (write(m->sock, &rq, sizeof rq) != (ssize_t)sizeof rq) { md_fail(dev, "request"); return; }
    m->state = 2; m->left = nchunks; m->chunk = chunk;
}
int mapd_chunk(int dev, void **handle)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ) return 0;
    struct md *m = &g_md[dev]; if (m->state != 2 || m->left <= 0) return 0;
    double t0 = now(); struct pollfd pf = { m->sock, POLLIN, 0 };
    int pr; do { pr = poll(&pf, 1, MAPD_TIMEOUT_MS); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) { md_fail(dev, pr == 0 ? "timeout" : "poll"); return 0; }
    char tag = 0; struct iovec io; io.iov_base = &tag; io.iov_len = 1;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u; memset(&u, 0, sizeof u);
    struct msghdr mh; memset(&mh, 0, sizeof mh); mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = u.buf; mh.msg_controllen = sizeof u.buf;
    ssize_t r; do { r = recvmsg(m->sock, &mh, MSG_CMSG_CLOEXEC); } while (r < 0 && errno == EINTR);
    int fd = -1; struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (r == 1 && c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof(int));
    if (fd < 0) { md_fail(dev, r == 0 ? "helper exited" : tag == 'E' ? "create or export in the helper" : tag == 'F' ? "helper HIP init" : "no handle received"); return 0; }
    double t1 = now(); m->wait += t1 - t0;
    int cur = 0; (void)hipGetDevice(&cur); (void)hipSetDevice(dev);
    hipMemGenericAllocationHandle_t h = 0;
    hipError_t he = hipMemImportFromShareableHandle(&h, (void *)(intptr_t)fd, FDT); close(fd);
    (void)hipSetDevice(cur);
    if (he != hipSuccess) { md_fail(dev, "import"); return 0; }
    m->imp += now() - t1; m->n++;
    if (--m->left == 0) { m->state = 3; int st; if (waitpid(m->pid, &st, WNOHANG) == m->pid) m->pid = 0; }   /* (the helper exits by itself after its last chunk) */
    *handle = (void *)h;
    return 1;
}
int mapd_stat(int dev, int *n, double *wait, double *imp)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ || !g_md[dev].n) return 0;
    *n = g_md[dev].n; *wait = g_md[dev].wait; *imp = g_md[dev].imp; return 1;
}
void mapd_finish(int dev)
{
    if (!g_on || dev < 0 || dev >= MAPD_NQ) return;
    struct md *m = &g_md[dev];
    if (m->sock >= 0) { close(m->sock); m->sock = -1; }
    if (m->pid > 0) { int st; pid_t r = waitpid(m->pid, &st, WNOHANG); if (r == 0) { kill(m->pid, SIGKILL); waitpid(m->pid, &st, 0); } m->pid = 0; }
    m->state = 3;
}
