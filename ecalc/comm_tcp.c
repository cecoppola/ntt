/* comm_tcp.c - the WP6 communicator: plain TCP sockets, full mesh, one process
 * per rank.  Correctness only (aac6 has 1 GbE and no RDMA); the target
 * system gets a Slingshot implementation of the same comm_ops.
 *
 * Environment: COMM_RANK, COMM_SIZE, COMM_HOSTS (comma-separated host per rank),
 * COMM_PORT (base port, default 27000; rank r listens on base + r).
 * Rank r accepts connections from ranks < r and connects to ranks > r.
 * all-to-all: device slabs are staged through pinned host buffers; one
 * sender thread per peer writes its slab while a receiver thread reads
 * from every peer in turn (sends and receives overlap, so blocking sockets
 * cannot deadlock); alltoall returns once the send slabs are staged (M7: the
 * caller's GPU work and the layered communicator's xGMI stage of the next
 * slab run under the transfer).  wait() joins the threads and uploads the
 * received slabs.  allgather: the same with one block sent to every peer;
 * blocks up to 4 KiB (the flags and descriptors) go without threads.
 *
 * Build with -DCOMM_HOST_ONLY to use host memory and memcpy instead of HIP
 * (for testing on a machine without GPUs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "comm.h"
#ifdef COMM_HOST_ONLY
#define DEV_TO_HOST(d, s, n, st) memcpy(d, s, n)
#define HOST_TO_DEV(d, s, n, st) memcpy(d, s, n)
#define HOST_ALLOC(n) malloc(n)
#define HOST_FREE(p) free(p)
#else
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define DEV_TO_HOST(d, s, n, st) do { HIP_CHECK(hipMemcpyAsync(d, s, n, hipMemcpyDeviceToHost, st)); HIP_CHECK(hipStreamSynchronize(st)); } while (0)
#define HOST_TO_DEV(d, s, n, st) do { HIP_CHECK(hipMemcpyAsync(d, s, n, hipMemcpyHostToDevice, st)); HIP_CHECK(hipStreamSynchronize(st)); } while (0)
static void *host_alloc(size_t n) { void *p; HIP_CHECK(hipHostMalloc(&p, n, 0)); return p; }
#define HOST_ALLOC(n) host_alloc(n)
#define HOST_FREE(p) HIP_CHECK(hipHostFree(p))
#endif

typedef struct {
    int *fd;                    /* fd[r] socket to rank r (-1 for self) */
    char *hs, *hr;              /* host staging: size slabs each */
    size_t cap;                 /* bytes per slab the staging can hold */
    pthread_t *th, rth; int *th_peer; size_t bytes; int pending;
    void *rb_dev; hipStream_t st;
} tcp_priv;
#define PRIV(c) ((tcp_priv *)(c)->priv)

static void die(const char *what) { perror(what); exit(1); }
static void write_all(int fd, const void *b, size_t n)
{
    const char *p = (const char *)b;
    while (n) { ssize_t k = write(fd, p, n); if (k < 0) { if (errno == EINTR) continue; die("write"); } p += k; n -= (size_t)k; }
}
static void read_all(int fd, void *b, size_t n)
{
    char *p = (char *)b;
    while (n) { ssize_t k = read(fd, p, n); if (k < 0) { if (errno == EINTR) continue; die("read"); } if (k == 0) { fprintf(stderr, "comm_tcp: peer closed\n"); exit(1); } p += k; n -= (size_t)k; }
}
static int t_rank(comm *c) { return c->rank; }
static int t_size(comm *c) { return c->size; }

typedef struct { int fd; const char *buf; size_t n; } send_job;
static void *sender(void *a) { send_job *j = (send_job *)a; write_all(j->fd, j->buf, j->n); free(j); return NULL; }
/* the receiver: from every peer in rank order (sender r writes only to us on fd[r], so order is irrelevant) */
typedef struct { comm *c; char *dst; size_t bytes; } recv_job;
static void *receiver(void *a)
{
    recv_job *j = (recv_job *)a; tcp_priv *p = PRIV(j->c); int n = j->c->size, me = j->c->rank;
    for (int r = 0; r < n; r++) if (r != me) read_all(p->fd[r], j->dst + (size_t)r * j->bytes, j->bytes);
    free(j); return NULL;
}
static void staging(tcp_priv *p, int n, size_t bytes)
{
    if (bytes <= p->cap) return;
    if (p->hs) { HOST_FREE(p->hs); HOST_FREE(p->hr); }
    p->cap = bytes; p->hs = (char *)HOST_ALLOC(bytes * n); p->hr = (char *)HOST_ALLOC(bytes * n);
}
/* the threads of one exchange: peer r gets src + stride r (stride 0: the same block to everyone), dst receives [r][bytes] */
static void exchange_start(comm *c, const char *src, size_t stride, char *dst, size_t bytes)
{
    tcp_priv *p = PRIV(c); int n = c->size, me = c->rank, k = 0;
    for (int r = 0; r < n; r++) if (r != me) {
        send_job *j = (send_job *)malloc(sizeof *j); j->fd = p->fd[r]; j->buf = src + stride * (size_t)r; j->n = bytes;
        if (pthread_create(&p->th[k], NULL, sender, j)) die("pthread_create");
        p->th_peer[k++] = r;
    }
    recv_job *j = (recv_job *)malloc(sizeof *j); j->c = c; j->dst = dst; j->bytes = bytes;
    if (pthread_create(&p->rth, NULL, receiver, j)) die("pthread_create");
}
static void exchange_join(comm *c)
{
    tcp_priv *p = PRIV(c);
    for (int k = 0; k < c->size - 1; k++) pthread_join(p->th[k], NULL);
    pthread_join(p->rth, NULL);
}
static void t_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    tcp_priv *p = PRIV(c); int n = c->size, me = c->rank;
    if (p->pending) { fprintf(stderr, "comm_tcp: alltoall while one is pending\n"); exit(1); }
    staging(p, n, bytes);
    DEV_TO_HOST(p->hs, sb, bytes * n, s);
    p->bytes = bytes; p->rb_dev = rb; p->st = s; p->pending = 1;
    memcpy(p->hr + (size_t)me * bytes, p->hs + (size_t)me * bytes, bytes);   /* self slab */
    exchange_start(c, p->hs, bytes, p->hr, bytes);
}
static void t_wait(comm *c)
{
    tcp_priv *p = PRIV(c);
    if (!p->pending) return;
    exchange_join(c);
    HOST_TO_DEV(p->rb_dev, p->hr, p->bytes * c->size, p->st);
    p->pending = 0;
}
/* all-gather of host blocks: below 4 KiB write to all then read from all (never fills a socket buffer); else threads */
static void t_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    tcp_priv *p = PRIV(c); int n = c->size, me = c->rank;
    if (p->pending) { fprintf(stderr, "comm_tcp: allgather while an all-to-all is pending\n"); exit(1); }
    char *self = (char *)rb + (size_t)me * bytes;
    if (self != sb) memcpy(self, sb, bytes);
    if (bytes <= 4096) {
        for (int r = 0; r < n; r++) if (r != me) write_all(p->fd[r], sb, bytes);
        for (int r = 0; r < n; r++) if (r != me) read_all(p->fd[r], (char *)rb + (size_t)r * bytes, bytes);
        return;
    }
    exchange_start(c, (const char *)sb, 0, (char *)rb, bytes); exchange_join(c);
}
/* device blocks: staged through the host buffers (hs: my block; hr: size blocks) */
static void t_allgather(comm *c, const void *sb, void *rb, size_t bytes)
{
    tcp_priv *p = PRIV(c); int n = c->size;
    if (p->pending) { fprintf(stderr, "comm_tcp: allgather while an all-to-all is pending\n"); exit(1); }
    staging(p, n, bytes);
    DEV_TO_HOST(p->hs, sb, bytes, 0);
    t_allgather_host(c, p->hs, p->hr, bytes);
    HOST_TO_DEV(rb, p->hr, bytes * n, 0);
}
/* small exchanges (barrier, reductions): a host-side all-to-all of 8-byte values, no threads needed
 * because 8 bytes never fill a socket buffer */
static void tiny_exchange(comm *c, uint64_t v, uint64_t *all)
{
    tcp_priv *p = PRIV(c); int n = c->size, me = c->rank;
    all[me] = v;
    for (int r = 0; r < n; r++) if (r != me) write_all(p->fd[r], &v, 8);
    for (int r = 0; r < n; r++) if (r != me) read_all(p->fd[r], &all[r], 8);
}
static void t_barrier(comm *c) { uint64_t all[4096]; tiny_exchange(c, 0, all); }
static uint64_t mulmod128(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)((unsigned __int128)a * b % q); }
static uint64_t t_modq(comm *c, uint64_t v, uint64_t q, uint64_t w)
{
    uint64_t all[4096], acc = 0, wr = 1;
    tiny_exchange(c, v, all);
    for (int r = 0; r < c->size; r++) { acc = (acc + mulmod128(all[r] % q, wr, q)) % q; wr = mulmod128(wr, w % q, q); }
    return acc;
}
static size_t t_max2(comm *c, size_t v)
{
    uint64_t all[4096], m = v; tiny_exchange(c, v, all);
    for (int r = 0; r < c->size; r++) if (all[r] > m) m = all[r];
    return (size_t)m;
}
static void t_destroy(comm *c)
{
    tcp_priv *p = PRIV(c);
    for (int r = 0; r < c->size; r++) if (p->fd[r] >= 0) close(p->fd[r]);
    if (p->hs) { HOST_FREE(p->hs); HOST_FREE(p->hr); }
    free(p->fd); free(p->th); free(p->th_peer); free(p); free(c);
}
static void t_send(comm *c, int to, const void *b, size_t n) { tcp_priv *p = (tcp_priv *)c->priv; write_all(p->fd[to], b, n); }
static void t_recv(comm *c, int from, void *b, size_t n) { tcp_priv *p = (tcp_priv *)c->priv; read_all(p->fd[from], b, n); }
static const struct comm_ops tcp_ops = { t_rank, t_size, t_alltoall, t_wait, t_barrier, t_modq, t_max2, t_destroy, t_send, t_recv, t_allgather, t_allgather_host };

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) die("socket");
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) die("bind");
    if (listen(fd, 64) < 0) die("listen");
    return fd;
}
static int connect_to(const char *host, int port)
{
    struct addrinfo hints, *res; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res)) { fprintf(stderr, "comm_tcp: cannot resolve %s\n", host); exit(1); }
    int fd = -1;
    for (int tries = 0; tries < 6000; tries++) {                /* peers may start later (M3: a level's group meshes open when its members arrive): retry for 600 s */
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) break;
        close(fd); fd = -1; usleep(100000);
    }
    freeaddrinfo(res);
    if (fd < 0) { fprintf(stderr, "comm_tcp: cannot connect to %s:%d\n", host, port); exit(1); }
    return fd;
}
static void set_opts(int fd) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }

comm *comm_tcp_create(void)
{
    const char *er = getenv("COMM_RANK"), *es = getenv("COMM_SIZE"), *eh = getenv("COMM_HOSTS"), *ep = getenv("COMM_PORT");
    if (!er || !es || !eh) { fprintf(stderr, "comm_tcp: need COMM_RANK, COMM_SIZE, COMM_HOSTS\n"); exit(1); }
    return comm_tcp_create_at(atoi(er), atoi(es), eh, ep ? atoi(ep) : 27000);
}
/* the mesh for rank me of n at the given hosts (comma list of n names) and port base (rank r listens on base + r);
 * several meshes may live in one process on distinct port bases (Phase 8 M1: one per APU thread) */
comm *comm_tcp_create_at(int me, int n, const char *eh, int base)
{
    if (n < 1 || n > 4096 || me < 0 || me >= n) { fprintf(stderr, "comm_tcp: bad rank/size\n"); exit(1); }
    char *hosts = strdup(eh), *host[4096]; int nh = 0;
    char *sp; for (char *t = strtok_r(hosts, ",", &sp); t && nh < n; t = strtok_r(NULL, ",", &sp)) host[nh++] = t;   /* (strtok_r: four meshes are created by four threads at once) */
    if (nh != n) { fprintf(stderr, "comm_tcp: COMM_HOSTS lists %d hosts for size %d\n", nh, n); exit(1); }
    comm *c = (comm *)calloc(1, sizeof *c); tcp_priv *p = (tcp_priv *)calloc(1, sizeof *p);
    c->ops = &tcp_ops; c->priv = p; c->rank = me; c->size = n; c->inflight = 1;
    p->fd = (int *)malloc(n * sizeof(int)); for (int r = 0; r < n; r++) p->fd[r] = -1;
    p->th = (pthread_t *)calloc(n, sizeof(pthread_t)); p->th_peer = (int *)calloc(n, sizeof(int));
    int lfd = me > 0 ? listen_on(base + me) : -1;
    /* connect upward first (higher ranks are listening), then accept from lower ranks */
    for (int r = me + 1; r < n; r++) { p->fd[r] = connect_to(host[r], base + r); set_opts(p->fd[r]); uint64_t id = (uint64_t)me; write_all(p->fd[r], &id, 8); }
    for (int k = 0; k < me; k++) {
        int fd = accept(lfd, NULL, NULL); if (fd < 0) die("accept");
        uint64_t id; read_all(fd, &id, 8);
        if (id >= (uint64_t)me) { fprintf(stderr, "comm_tcp: bad hello\n"); exit(1); }
        set_opts(fd); p->fd[id] = fd;
    }
    if (lfd >= 0) close(lfd);
    free(hosts);
    return c;
}
