/* fatal.h - one exit path for errors inside the run (Phase 14 C3, PLAN.md §33 A3).
 *
 * Before: a check that failed inside a phase called exit(1) (or abort()) from whichever thread found it.  The four APU worker
 * threads (OpenMP) often find the same condition together (the in-phase pool guards, an allocation that fails on every APU);
 * several exit() calls at once run the atexit handlers and the C++ destructors of the HIP runtime concurrently with each other
 * and with the threads still computing, and the process died with SIGSEGV (rc 139) instead of the message (G13d §(c),
 * results/L114.md batch 6).
 *
 * Now every such site calls ec_fatal() (or ec_fatal_begin() ... ec_fatal_end() when it prints more, e.g. the memory report):
 *   - the FIRST caller in the process prints one line "ecalc: FATAL [rank r/n host h, APU d, thread t, after <phase>, rc c]:
 *     <what failed, with the sizes>", runs its extra reporting, flushes stdout and stderr and leaves with _exit(code): no
 *     atexit handler, no destructor, no race;
 *   - every LATER caller (another worker thread that found the same, or any thread after the first) blocks for good and prints
 *     nothing; the process is gone within the first caller's reporting time;
 *   - a watchdog started by the first caller _exit()s after ECALC_FATAL_WAIT seconds (default 60) in case its reporting
 *     blocks (a lock held by a thread that is itself parked in ec_fatal);
 *   - a re-entry from the first caller's own thread (a failure inside its reporting) _exit()s at once with the same code.
 * ECALC_FATAL_ABORT=1: abort() instead of _exit (a core file for a debugger; rc 134).
 *
 * Exit codes (distinct from a signal's 128 + n and from the usage errors' 2): */
#ifndef EC_FATAL_H
#define EC_FATAL_H
#ifdef __cplusplus
extern "C" {
#endif

#define EC_RC_FATAL  3   /* a check failed inside the run (was exit(1) or abort()): an internal error, a HIP call, a transport */
#define EC_RC_OOM    6   /* an allocation failed, or a pool would grow inside a phase (the in-phase guards) */
#define EC_RC_BUDGET 8   /* ECALC_BUDGET_CHECK=1: a node's modelled peak exceeds its budget; every rank stops before compute */

void ec_fatal(int code, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
/* the two-part form: ec_fatal_begin prints the header line and the message (and blocks a later caller), the caller then prints
 * what it wants (mem_report ...) and calls ec_fatal_end, which flushes and exits */
void ec_fatal_begin(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ec_fatal_end(int code) __attribute__((noreturn));
/* the phase the header names: mem_report() records its boundary here ("after init" ...); anything may set it */
void ec_fatal_phase(const char *name);
/* a clean stop of every rank (the budget check): flush, _exit(code), no atexit handler */
void ec_quit(int code) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
