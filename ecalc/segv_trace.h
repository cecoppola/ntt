/* segv_trace.h - s18w task 2: ECALC_SEGV_TRACE=1 (default 0) installs a SIGSEGV/SIGBUS/SIGABRT handler that prints
 * the rank/host, the signal, the faulting address and a raw-address backtrace of the faulting thread to stderr
 * with async-signal-safe calls only, then restores the default handler and re-raises so the process still dies
 * exactly as it would with the switch off (same exit status, same core-file behaviour).  Off by default: calling
 * ec_segv_trace_install() with ECALC_SEGV_TRACE unset or 0 is a single getenv() and nothing else changes.
 * ECALC_SEGV_TEST=1 (also default 0): once the handler is installed, raise a SIGSEGV immediately (host-only, no
 * GPU touched) so the handler's output can be checked without a real fault. */
#ifndef EC_SEGV_TRACE_H
#define EC_SEGV_TRACE_H
#ifdef __cplusplus
extern "C" {
#endif

void ec_segv_trace_install(void);

#ifdef __cplusplus
}
#endif
#endif
