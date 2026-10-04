/* segv2.c - LD_PRELOAD for the aac7 nodes (no gdb, root-only cores): a SIGSEGV/SIGBUS/SIGABRT handler that prints a backtrace
 * to stderr and KEEPS it -- sigaction()/signal() are interposed, so a library (libsma, PMI, libfabric) that installs its own
 * handler at init gets ours in front: theirs is recorded and chained after the backtrace.  Phase 16 S (results/S16.md).
 *   gcc -O1 -g -shared -fPIC -o segv2.so segv2.c -ldl
 * Prints: host, task, thread name, rip (with dladdr: the library and symbol), the maps lines of rip / the fault address / rsp --
 * without unwinding, which faulted on a foreign stack the first time (b1 ctrl_6: 64 recursive frames) -- then the backtrace.
 *   LD_PRELOAD=.../segv2.so <command>                SEGV2_TAG=<string> labels the lines (e.g. the task id) */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <fcntl.h>
static struct sigaction g_theirs[65];
static int g_have[65];
static int ours(int s) { return s == SIGSEGV || s == SIGBUS || s == SIGABRT || s == SIGFPE || s == SIGILL; }
static void map_line(const char *what, unsigned long a)   /* the /proc/self/maps line holding address a (no unwinding needed) */
{
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    char l[512]; while (fgets(l, sizeof l, f)) { unsigned long lo, hi; if (sscanf(l, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) { write(2, "segv2.so: ", 10); write(2, what, strlen(what)); write(2, " in ", 4); write(2, l, strlen(l)); break; } }
    fclose(f);
}
static volatile int g_depth;
static void h(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv; char m[512]; const char *tag = getenv("SEGV2_TAG"); char host[64] = "?", comm[32] = "?";
    int depth = __sync_add_and_fetch(&g_depth, 1);
    if (depth > 1) { int k = snprintf(m, sizeof m, "segv2.so: signal %d again inside the handler (depth %d, at %p): the backtrace itself faulted; stopping here\n", sig, depth, si->si_addr); write(2, m, k); fsync(2);
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = SIG_DFL; int (*real)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction"); real(sig, &sa, 0); raise(sig); return; }
    gethostname(host, sizeof host - 1);
    { int fd = open("/proc/thread-self/comm", O_RDONLY); if (fd >= 0) { int r = read(fd, comm, sizeof comm - 1); if (r > 0) { comm[r] = 0; if (comm[r - 1] == '\n') comm[r - 1] = 0; } close(fd); } }
    unsigned long rip = 0, rsp = 0;
#if defined(__x86_64__)
    rip = uc->uc_mcontext.gregs[REG_RIP]; rsp = uc->uc_mcontext.gregs[REG_RSP];
#endif
    int k = snprintf(m, sizeof m, "segv2.so[%s]: %s task %s: signal %d (%s) code %d at address %p, pid %d tid %ld thread '%s', rip 0x%lx rsp 0x%lx\n",
                     tag ? tag : "", host, getenv("SLURM_PROCID") ? getenv("SLURM_PROCID") : "?", sig, strsignal(sig), si->si_code, si->si_addr, (int)getpid(), (long)syscall(SYS_gettid), comm, rip, rsp);
    write(2, m, k);
    { Dl_info di; if (rip && dladdr((void *)rip, &di) && di.dli_fname) { k = snprintf(m, sizeof m, "segv2.so: rip in %s (%s+0x%lx)\n", di.dli_fname, di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? rip - (unsigned long)di.dli_saddr : rip - (unsigned long)di.dli_fbase); write(2, m, k); } }
    map_line("rip", rip); map_line("fault address", (unsigned long)si->si_addr); map_line("rsp", rsp);
    fsync(2);
    void *b[64]; int n = backtrace(b, 64);                 /* may fault on a foreign stack: the depth guard above stops the recursion */
    k = snprintf(m, sizeof m, "segv2.so: backtrace (%d frames)\n", n); write(2, m, k); backtrace_symbols_fd(b, n, 2); fsync(2);
    if (g_have[sig] && (g_theirs[sig].sa_flags & SA_SIGINFO) && g_theirs[sig].sa_sigaction) { write(2, "segv2.so: chaining to the library's handler\n", 44); g_theirs[sig].sa_sigaction(sig, si, ucv); return; }
    if (g_have[sig] && !(g_theirs[sig].sa_flags & SA_SIGINFO) && g_theirs[sig].sa_handler != SIG_DFL && g_theirs[sig].sa_handler != SIG_IGN) { g_theirs[sig].sa_handler(sig); return; }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = SIG_DFL;
    int (*real)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction"); real(sig, &sa, 0); raise(sig);
}
static void install(void)
{
    static int done; if (done) return; done = 1;
    int (*real)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction");
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;   /* NODEFER: a fault inside the handler (backtrace on a foreign stack) re-enters it; the depth guard stops at 2 */
    static char stk[1 << 17]; stack_t ss = { .ss_sp = stk, .ss_size = sizeof stk }; sigaltstack(&ss, 0);
    for (int s = 1; s < 65; s++) if (ours(s)) real(s, &sa, 0);
}
__attribute__((constructor)) static void init(void) { install(); }
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    int (*real)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction");
    if (sig > 0 && sig < 65 && ours(sig)) {
        install();
        if (old) { if (g_have[sig]) *old = g_theirs[sig]; else { memset(old, 0, sizeof *old); old->sa_handler = SIG_DFL; } }
        if (act) { g_theirs[sig] = *act; g_have[sig] = 1;
            char m[128]; int k = snprintf(m, sizeof m, "segv2.so: sigaction(%d) by the program/library intercepted (handler %p, flags 0x%x): ours stays in front\n", sig, (act->sa_flags & SA_SIGINFO) ? (void *)act->sa_sigaction : (void *)act->sa_handler, (unsigned)act->sa_flags); write(2, m, k); }
        return 0;
    }
    return real(sig, act, old);
}
typedef void (*sighandler_t_)(int);
sighandler_t_ signal(int sig, sighandler_t_ f)
{
    if (sig > 0 && sig < 65 && ours(sig)) { struct sigaction sa, old; memset(&sa, 0, sizeof sa); sa.sa_handler = f; sigaction(sig, &sa, &old); return old.sa_handler; }
    sighandler_t_ (*real)(int, sighandler_t_) = dlsym(RTLD_NEXT, "signal"); return real(sig, f);
}
