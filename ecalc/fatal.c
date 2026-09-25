/* fatal.c - one exit path for errors inside the run (Phase 14 C3; fatal.h has the rules).
 * Built with hipcc (the APU of the calling thread from hipGetDevice); -DEC_FATAL_NO_HIP for the host-only transport test. */
#include "fatal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#ifndef EC_FATAL_NO_HIP
#include <hip/hip_runtime_api.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif

static int g_state;                 /* 0 no failure yet, 1 a failure is being reported */
static long g_owner;                /* the reporting thread's tid */
static int g_code;
static char g_phase[48] = "start";

void ec_fatal_phase(const char *name) { if (name) snprintf(g_phase, sizeof g_phase, "%s", name); }

static long tid_now(void) { return (long)syscall(SYS_gettid); }

static void leave(int code)
{
    fflush(stdout); fflush(stderr);
    const char *a = getenv("ECALC_FATAL_ABORT");
    if (a && atoi(a)) abort();
    _exit(code);
}

void ec_quit(int code) { leave(code); }

static void *watchdog(void *arg)
{
    const char *e = getenv("ECALC_FATAL_WAIT"); int s = e ? atoi(e) : 60; if (s < 1) s = 1;
    sleep((unsigned)s);
    fprintf(stderr, "ecalc: FATAL: the report did not finish in %d s (ECALC_FATAL_WAIT); exiting with rc %d\n", s, (int)(long)arg);
    leave((int)(long)arg);
    return 0;
}

/* returns only in the first caller; a later caller blocks, the first caller's own re-entry exits */
static void claim(int code)
{
    int exp = 0;
    if (!__atomic_compare_exchange_n(&g_state, &exp, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        if (__atomic_load_n(&g_owner, __ATOMIC_SEQ_CST) == tid_now()) { fprintf(stderr, "ecalc: FATAL: a second failure while reporting the first (rc %d)\n", g_code); leave(g_code); }
        for (;;) pause();          /* another thread is reporting and will _exit the process */
    }
    __atomic_store_n(&g_owner, tid_now(), __ATOMIC_SEQ_CST); g_code = code;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    (void)pthread_create(&th, &at, watchdog, (void *)(long)code);
}

static void header(int code, const char *fmt, va_list ap)
{
    char host[64] = "?"; gethostname(host, sizeof host); host[sizeof host - 1] = 0;
    const char *r = getenv("COMM_RANK"), *s = getenv("COMM_SIZE");
    int dev = -1;
#ifndef EC_FATAL_NO_HIP
    if (hipGetDevice(&dev) != hipSuccess) dev = -1;
#endif
    int th = -1;
#ifdef _OPENMP
    th = omp_get_thread_num();
#endif
    char msg[2048]; vsnprintf(msg, sizeof msg, fmt, ap);
    size_t n = strlen(msg); while (n && msg[n - 1] == '\n') msg[--n] = 0;
    fflush(stdout);
    fprintf(stderr, "ecalc: FATAL [rank %s/%s host %s, APU %d, thread %d (tid %ld), after %s, rc %d]: %s\n",
            r ? r : "0", s ? s : "1", host, dev, th, tid_now(), g_phase, code, msg);
    fflush(stderr);
}

void ec_fatal_begin(int code, const char *fmt, ...)
{
    claim(code);
    va_list ap; va_start(ap, fmt); header(code, fmt, ap); va_end(ap);
}

void ec_fatal_end(int code) { leave(code); }

void ec_fatal(int code, const char *fmt, ...)
{
    claim(code);
    va_list ap; va_start(ap, fmt); header(code, fmt, ap); va_end(ap);
    leave(code);
}
