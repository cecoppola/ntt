/* fi_hmem_shim.c: LD_PRELOAD shim for libsma on aac7 (benchmark only, branch devsrc2).
 * libsma imports fi_getinfo@FABRIC_1.3 (and asks for API version 1.5); this wrapper adds FI_HMEM to the hints' caps before the provider is selected,
 * so that the cxi provider re-detects device pointers (ROCR / dmabuf) when the library registers memory.
 *   FI_SHIM_MODE=A: caps |= FI_HMEM.   B: also domain_attr->mr_mode |= FI_MR_HMEM.   Unset or other: pass through.
 *   FI_SHIM_COPY=0 (default): modify the caller's hints in place and restore them after the call (no fi_dupinfo: libsma was built against an older
 *     struct layout); 1: fi_dupinfo copy, never freed; 2: fi_dupinfo copy, fi_freeinfo after the call (the first version, segfaulted in 9le).
 *   FI_SHIM_STRIP=1: clear FI_HMEM again in the returned fi_info list.   FI_SHIM_RETRY=1 (default): when the call with FI_HMEM fails or returns nothing, retry without.
 * Logs per process: before (once) and after the call (rc, provider, caps of the first returned info).  Build: cc -shared -fPIC -O1 -I<libfabric>/include -o fi_hmem_shim.so fi_hmem_shim.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <rdma/fabric.h>

typedef int (*getinfo_f)(uint32_t, const char *, const char *, uint64_t, const struct fi_info *, struct fi_info **);
typedef struct fi_info *(*dupinfo_f)(const struct fi_info *);
typedef void (*freeinfo_f)(struct fi_info *);
static void *sym(const char *n) { void *p = dlvsym(RTLD_NEXT, n, "FABRIC_1.3"); return p ? p : dlsym(RTLD_NEXT, n); }
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void say(const char *fmt, ...) { char b[400]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (n > 0) { if (n > (int)sizeof b - 1) n = sizeof b - 1; if (write(2, b, n) < 0) {} } }

int fi_getinfo(uint32_t version, const char *node, const char *service, uint64_t flags, const struct fi_info *hints, struct fi_info **info)
{
    static getinfo_f real; static dupinfo_f dup; static freeinfo_f fr; static int logged;
    if (!real) { real = (getinfo_f)sym("fi_getinfo"); dup = (dupinfo_f)sym("fi_dupinfo"); fr = (freeinfo_f)sym("fi_freeinfo"); }
    if (!real) { say("fi_hmem_shim: no real fi_getinfo\n"); return -FI_ENOSYS; }
    const char *m = getenv("FI_SHIM_MODE"), *c = getenv("FI_SHIM_COPY"), *s = getenv("FI_SHIM_STRIP"), *r = getenv("FI_SHIM_RETRY");
    int mode = m && (*m == 'A' || *m == 'B') ? *m : 0, copy = c ? atoi(c) : 0, strip = s ? atoi(s) : 0, retry = r ? atoi(r) : 1;
    if (!mode) return real(version, node, service, flags, hints, info);
    if (!logged) { logged = 1;
        say("fi_hmem_shim: pid %d mode %c copy %d strip %d version 0x%x hints %s (domain %s fabric %s prov %s)\n", (int)getpid(), mode, copy, strip, version, hints ? "given" : "NULL",
            hints && hints->domain_attr ? "yes" : "no", hints && hints->fabric_attr ? "yes" : "no", hints && hints->fabric_attr && hints->fabric_attr->prov_name ? hints->fabric_attr->prov_name : "-"); }
    struct fi_info *h = 0, *use = (struct fi_info *)hints; uint64_t caps0 = 0; uint32_t mr0 = 0; int rc;
    if (copy > 0 && dup) { h = dup(hints); use = h; }
    if (!use) { return real(version, node, service, flags, hints, info); }   /* NULL hints and no copy: leave it alone */
    caps0 = use->caps; if (use->domain_attr) mr0 = use->domain_attr->mr_mode;
    use->caps |= FI_HMEM; if (mode == 'B' && use->domain_attr) use->domain_attr->mr_mode |= FI_MR_HMEM;
    rc = real(version, node, service, flags, use, info);
    say("fi_hmem_shim: pid %d after call rc %d info %s prov %s caps 0x%llx\n", (int)getpid(), rc, rc == 0 && info && *info ? "yes" : "none",
        rc == 0 && info && *info && (*info)->fabric_attr && (*info)->fabric_attr->prov_name ? (*info)->fabric_attr->prov_name : "-", rc == 0 && info && *info ? (unsigned long long)(*info)->caps : 0ULL);
    use->caps = caps0; if (use->domain_attr) use->domain_attr->mr_mode = mr0;      /* restore the caller's (or the copy's) hints */
    if (strip && rc == 0 && info) for (struct fi_info *p = *info; p; p = p->next) p->caps &= ~(uint64_t)FI_HMEM;
    if (rc != 0 || !info || !*info) {
        if (retry) { say("fi_hmem_shim: pid %d FI_HMEM request failed (rc %d), retrying without\n", (int)getpid(), rc); rc = real(version, node, service, flags, use, info); }
    }
    if (h && copy == 2 && fr) fr(h);
    return rc;
}
