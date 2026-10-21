/* fi_hmem_shim.c: LD_PRELOAD shim for libsma on aac7 (benchmark only, branch devsrc2).
 * libsma imports fi_getinfo@FABRIC_1.3; this wrapper adds FI_HMEM to the hints' caps before the provider is selected, so that the cxi provider
 * re-detects device pointers (ROCR / dmabuf) when the library registers memory.
 *   FI_SHIM_MODE=A: caps |= FI_HMEM.   FI_SHIM_MODE=B: also domain_attr->mr_mode |= FI_MR_HMEM.   Unset or other: pass through.
 * Logs once per process to stderr.  Build: cc -shared -fPIC -O1 -I<libfabric>/include -o fi_hmem_shim.so fi_hmem_shim.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <rdma/fabric.h>

typedef int (*getinfo_f)(uint32_t, const char *, const char *, uint64_t, const struct fi_info *, struct fi_info **);
typedef struct fi_info *(*dupinfo_f)(const struct fi_info *);

int fi_getinfo(uint32_t version, const char *node, const char *service, uint64_t flags, const struct fi_info *hints, struct fi_info **info)
{
    static getinfo_f real; static dupinfo_f dup; static int logged;
    if (!real) { real = (getinfo_f)dlvsym(RTLD_NEXT, "fi_getinfo", "FABRIC_1.3"); dup = (dupinfo_f)dlvsym(RTLD_NEXT, "fi_dupinfo", "FABRIC_1.3"); }
    if (!real) { fprintf(stderr, "fi_hmem_shim: no real fi_getinfo\n"); return -FI_ENOSYS; }
    const char *m = getenv("FI_SHIM_MODE"); int mode = m && (*m == 'A' || *m == 'B') ? *m : 0;
    struct fi_info *h = 0; int rc;
    if (mode && dup) {
        h = dup(hints);                                   /* NULL hints: a fresh fi_info */
        if (h) { h->caps |= FI_HMEM;
            if (mode == 'B' && h->domain_attr) h->domain_attr->mr_mode |= FI_MR_HMEM; }
    }
    if (!logged) { logged = 1;
        fprintf(stderr, "fi_hmem_shim: pid %d mode %c hints %s version 0x%x prov %s: caps|=FI_HMEM%s\n", (int)getpid(), mode ? mode : '-', hints ? "given" : "NULL", version,
                hints && hints->fabric_attr && hints->fabric_attr->prov_name ? hints->fabric_attr->prov_name : "-", mode == 'B' ? " mr_mode|=FI_MR_HMEM" : ""); }
    rc = real(version, node, service, flags, h ? h : hints, info);
    if (h) { void (*fr)(struct fi_info *) = (void (*)(struct fi_info *))dlvsym(RTLD_NEXT, "fi_freeinfo", "FABRIC_1.3"); if (fr) fr(h); }
    return rc;
}
