/* fatal.h - one exit path for errors inside the run. A failing check must not call exit(1)/abort() from whichever
 * thread found it: the four APU worker threads often find the same condition together, and concurrent exit() runs the
 * atexit handlers and HIP runtime destructors against each other and against threads still computing (SIGSEGV, rc
 * 139, instead of the message). Every such site calls ec_fatal() (or ec_fatal_begin() ... ec_fatal_end() when it
 * prints more, e.g. the memory report):
 * - the FIRST caller prints one line "ecalc: FATAL [rank r/n host h, APU d, thread t, after <phase>, rc c]: <what
 * failed, with the sizes>", runs its extra reporting, flushes, and leaves with _exit(code): no atexit handler, no
 * destructor, no race;
 * - every LATER caller blocks for good and prints nothing;
 * - a watchdog started by the first caller _exit()s after 60 seconds in case its reporting blocks;
 * - a re-entry from the first caller's own thread (failure inside its reporting) _exit()s at once with the same code.
 * Exit codes (distinct from a signal's 128 + n and from the usage errors' 2): */
#ifndef EC_FATAL_H
#define EC_FATAL_H
#ifdef __cplusplus
extern "C" {
#endif

#define EC_RC_FATAL  3   /* a check failed inside the run (not exit(1) or abort()): an internal error, a HIP call, a transport */
#define EC_RC_OOM    6   /* an allocation failed, or a pool would grow inside a phase (the in-phase guards) */
#define EC_RC_BUDGET 8   /* ECALC_BUDGET_CHECK=1: a node's modelled peak exceeds its budget; every rank stops before compute */

void ec_fatal(int code, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
/* two-part form: ec_fatal_begin prints the header and the message (and blocks a later caller); the caller then prints
 * what it wants and calls ec_fatal_end, which flushes and exits */
void ec_fatal_begin(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ec_fatal_end(int code) __attribute__((noreturn));
/* the phase the header names (mem_report() records its boundary here); anything may set it */
void ec_fatal_phase(const char *name);
/* clean stop of every rank (the budget check): flush, _exit(code), no atexit handler */
void ec_quit(int code) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
