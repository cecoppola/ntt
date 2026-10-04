/* segv2.c - LD_PRELOAD for the aac7 nodes (no gdb, root-only cores): a SIGSEGV/SIGBUS/SIGABRT handler that prints a backtrace
 * to stderr and KEEPS it -- sigaction()/signal() are interposed, so a library (libsma, PMI, libfabric) that installs its own
 * handler at init gets ours in front: theirs is recorded and chained after the backtrace.  Phase 16 S (results/S16.md).
 *   gcc -O1 -g -shared -fPIC -o segv2.so segv2.c -ldl
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
static struct sigaction g_theirs[65];
static int g_have[65];
static int ours(int s) { return s == SIGSEGV || s == SIGBUS || s == SIGABRT || s == SIGFPE || s == SIGILL; }
static void h(int sig, siginfo_t *si, void *uc)
{
    void *b[64]; int n = backtrace(b, 64);
    char m[256]; const char *tag = getenv("SEGV2_TAG");
    int k = snprintf(m, sizeof m, "segv2.so[%s]: signal %d (%s) code %d at address %p, pid %d tid %ld: backtrace (%d frames)\n",
                     tag ? tag : "", sig, strsignal(sig), si->si_code, si->si_addr, (int)getpid(), (long)syscall(SYS_gettid), n);
    write(2, m, k); backtrace_symbols_fd(b, n, 2);
    /* the mappings around the fault address, for the diagnosis of a VMM / registration range */
    { FILE *f = fopen("/proc/self/maps", "r"); if (f) { char l[512]; unsigned long a = (unsigned long)si->si_addr; int printed = 0;
        while (fgets(l, sizeof l, f)) { unsigned long lo, hi; if (sscanf(l, "%lx-%lx", &lo, &hi) == 2 && a >= lo - (1ul << 30) && a < hi + (1ul << 30)) { write(2, "segv2.so: map ", 14); write(2, l, strlen(l)); if (++printed > 12) break; } }
        fclose(f); } }
    fsync(2);
    if (g_have[sig] && (g_theirs[sig].sa_flags & SA_SIGINFO) && g_theirs[sig].sa_sigaction) { write(2, "segv2.so: chaining to the library's handler\n", 44); g_theirs[sig].sa_sigaction(sig, si, uc); return; }
    if (g_have[sig] && !(g_theirs[sig].sa_flags & SA_SIGINFO) && g_theirs[sig].sa_handler != SIG_DFL && g_theirs[sig].sa_handler != SIG_IGN) { g_theirs[sig].sa_handler(sig); return; }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = SIG_DFL;
    int (*real)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction"); real(sig, &sa, 0); raise(sig);
}
static void install(void)
{
    static int done; if (done) return; done = 1;
    int (*real)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction");
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
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
