/* segv_trace.h - ECALC_SEGV_TRACE=1 (default 0) installs a SIGSEGV/SIGBUS/SIGABRT handler that prints rank/host,
 * signal, faulting address and a raw-address backtrace of the faulting thread to stderr (async-signal-safe calls
 * only), then restores the default handler and re-raises, so the process dies exactly as with the switch off. With
 * the switch unset or 0, ec_segv_trace_install() is a single getenv(). */
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
