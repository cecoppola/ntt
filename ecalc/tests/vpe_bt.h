/* vpe_bt.h - the --vpe tests: on SIGFPE / SIGSEGV / SIGABRT print the faulting thread's backtrace as "BT <module> <offset>" lines (stderr, unbuffered; the item runs
 * addr2line on them), then die with the same signal.  A crash with stdout redirected loses the buffered lines; this keeps the place. */
#ifndef EC_VPE_BT_H
#define EC_VPE_BT_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <execinfo.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void vpe_bt_handler(int sig)
{
    void *bt[48]; int n = backtrace(bt, 48); char b[256];
    int k = snprintf(b, sizeof b, "BT signal %d\n", sig); if (write(2, b, k) < 0) _exit(1);
    for (int i = 0; i < n; i++) {
        Dl_info di; if (!dladdr(bt[i], &di) || !di.dli_fname) continue;
        k = snprintf(b, sizeof b, "BT %s 0x%lx\n", di.dli_fname, (unsigned long)((char *)bt[i] - (char *)di.dli_fbase)); if (write(2, b, k) < 0) _exit(1);
    }
    signal(sig, SIG_DFL); raise(sig);
}
__attribute__((constructor)) static void vpe_bt_install(void) { signal(SIGFPE, vpe_bt_handler); signal(SIGSEGV, vpe_bt_handler); signal(SIGABRT, vpe_bt_handler); }
#endif
