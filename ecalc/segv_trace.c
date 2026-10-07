/* segv_trace.c - ECALC_SEGV_TRACE=1 (default 0, segv_trace.h has the rules).  Built like fatal.c (hipcc -x hip; see
 * the Makefile's segv_trace.o rule) so a C++ exception thrown through a HIP translation unit still links; nothing
 * here needs C++, it is plain C compiled under that front end.
 *
 * Why not -rdynamic: it only widens the dynamic symbol table (every global symbol gets a .dynsym entry instead of
 * just the ones something outside the binary already needs), which does not touch codegen, floating point or
 * memory layout -- but ecalc's Makefile links through hipcc with HIP device code and vendor runtimes already on
 * the link line, and this task does not need to take that risk to get a usable trace offline: backtrace_symbols_fd()
 * still resolves whatever the normal dynamic symbol table carries (library frames typically resolve by name; this
 * binary's own non-exported static functions would not, with or without -rdynamic, unless the function happens to
 * be externally visible already), and the raw addresses plus the /proc/self/maps line for ecalc's own text mapping
 * (printed below) are enough for an offline `addr2line -e ecalc <addr - load base>` pass with the project's own
 * -g (HIPFLAGS already has it).  So: no Makefile change, no link-line risk, maps line + raw addresses instead.
 *
 * Async-signal-safety inside the handler: only write(2), read(2) on /proc/self/maps, close(2), sigaction(2),
 * raise(2) and backtrace()/backtrace_symbols_fd() (glibc document backtrace_symbols_fd as fork/signal safe -- it
 * writes directly to the fd, no malloc -- unlike backtrace_symbols(), which does malloc and is NOT used here).
 * backtrace() itself can lazily dlopen/malloc on its very first call in the process, so ec_segv_trace_install()
 * below makes one throwaway call before anything can fault, to force that lazy init outside the handler.  No
 * printf/snprintf/getenv/gethostname/strstr/readlink inside the handler: those run once at install time instead
 * and are cached into static buffers the handler only reads. */
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <execinfo.h>
#include <fcntl.h>
#include "segv_trace.h"

#define EC_MAX_FRAMES 64
#define EC_EXE_MAX 4096

static char g_host[64];
static char g_rank[16];
static char g_size[16];
static char g_exe[EC_EXE_MAX];      /* /proc/self/exe's target, cached at install time (readlink is not on the
                                        standard async-signal-safe list; calling it once outside the handler avoids
                                        the question entirely) */
static int g_exe_len;
static void *g_warm[1];             /* the install-time throwaway backtrace() call's frame buffer */

/* ---- async-signal-safe output helpers (write(2) only, no buffering, no malloc) ---- */

static void wr(int fd, const char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n) { ssize_t w = write(fd, s, n); if (w <= 0) break; s += (size_t)w; n -= (size_t)w; }
}

static void wr_long(int fd, long v)
{
    char buf[32]; int i = (int)sizeof buf; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 0L)) : (unsigned long)v;   /* v == LONG_MIN is never passed here (signal numbers only) */
    buf[--i] = 0;
    do { buf[--i] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    if (neg) buf[--i] = '-';
    wr(fd, buf + i);
}

static void wr_hex(int fd, unsigned long v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[2 + sizeof(unsigned long) * 2]; int i = (int)sizeof buf; buf[--i] = 0;
    do { buf[--i] = hexd[v & 0xful]; v >>= 4; } while (v);
    buf[--i] = 'x'; buf[--i] = '0';
    wr(fd, buf + i);
}

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGABRT: return "SIGABRT";
    default:      return "signal";
    }
}

/* the /proc/self/maps lines whose path field is this executable (not a shared library): read(2) + write(2) only,
 * a fixed line buffer, no malloc -- safe to call from the handler */
static void maps_dump(int fd)
{
    if (!g_exe_len) return;
    int mf = open("/proc/self/maps", O_RDONLY);
    if (mf < 0) return;
    wr(fd, "ecalc: SEGV: /proc/self/maps lines for "); wr(fd, g_exe); wr(fd, " (offline: addr2line -e <binary> <addr - load base>):\n");
    char buf[4096]; char line[1024]; size_t lp = 0; ssize_t r;
    while ((r = read(mf, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n' || lp == sizeof line - 1) {
                line[lp] = 0;
                if (lp >= (size_t)g_exe_len) {
                    /* crude substring search, no strstr (not on the strict async-signal-safe list; this one only
                       touches the fixed local buffer, no shared state, so it is kept, but written out longhand) */
                    int found = 0;
                    for (size_t off = 0; off + (size_t)g_exe_len <= lp && !found; off++) {
                        if (memcmp(line + off, g_exe, (size_t)g_exe_len) == 0) found = 1;
                    }
                    if (found) { wr(fd, line); wr(fd, "\n"); }
                }
                lp = 0;
            } else line[lp++] = c;
        }
    }
    close(mf);
}

static void handler(int sig, siginfo_t *info, void *uctx)
{
    (void)uctx;
    int fd = 2;
    wr(fd, "ecalc: SEGV [rank "); wr(fd, g_rank); wr(fd, "/"); wr(fd, g_size); wr(fd, " host "); wr(fd, g_host); wr(fd, "]");
    wr(fd, " signal "); wr(fd, sig_name(sig)); wr(fd, " ("); wr_long(fd, sig); wr(fd, ")");
    if (info && (sig == SIGSEGV || sig == SIGBUS)) { wr(fd, " addr "); wr_hex(fd, (unsigned long)info->si_addr); }
    wr(fd, "\n");

    void *frames[EC_MAX_FRAMES];
    int nf = backtrace(frames, EC_MAX_FRAMES);
    wr(fd, "ecalc: SEGV: backtrace of the faulting thread (raw return addresses; resolve offline with the maps line below):\n");
    if (nf > 0) backtrace_symbols_fd(frames, nf, fd);

    maps_dump(fd);

    /* restore the signal's default disposition and re-raise it: the process dies exactly as it would with
     * ECALC_SEGV_TRACE=0 (same exit status / core-file behaviour) -- this handler only adds the printing above */
    struct sigaction def; memset(&def, 0, sizeof def);
    def.sa_handler = SIG_DFL;
    sigemptyset(&def.sa_mask);
    sigaction(sig, &def, 0);
    raise(sig);
}

/* ECALC_SEGV_TEST=1: a host-only SIGSEGV right after the handler is installed, before any GPU use -- exercises the
 * handler on the login node with no srun, no GPU, matching the s18w task's test instructions */
static void test_trigger(void)
{
    volatile int *p = 0;
    *p = 1;
}

void ec_segv_trace_install(void)
{
    const char *e = getenv("ECALC_SEGV_TRACE");
    if (!e || !atoi(e)) return;    /* default: no-op other than this one getenv() -- no handler, no behaviour change */

    g_host[0] = 0; gethostname(g_host, sizeof g_host); g_host[sizeof g_host - 1] = 0;
    const char *r = getenv("COMM_RANK"), *s = getenv("COMM_SIZE");
    snprintf(g_rank, sizeof g_rank, "%s", r ? r : "0");
    snprintf(g_size, sizeof g_size, "%s", s ? s : "1");

    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
    g_exe_len = n > 0 ? (int)n : 0;
    if (g_exe_len) g_exe[g_exe_len] = 0; else g_exe[0] = 0;

    backtrace(g_warm, 1);           /* force backtrace()'s lazy first-use init (can malloc/dlopen) here, not in the handler */

    struct sigaction act; memset(&act, 0, sizeof act);
    act.sa_sigaction = handler;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    sigaction(SIGSEGV, &act, 0);
    sigaction(SIGBUS, &act, 0);
    sigaction(SIGABRT, &act, 0);

    if (getenv("ECALC_SEGV_TEST") && atoi(getenv("ECALC_SEGV_TEST"))) test_trigger();
}
