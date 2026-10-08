/* S31: the comm_ofi pool allocator under the X2 pattern (v-slots regrown in the pool between short-lived staging blocks).
 * Host-only (no NIC): comm_ofi.c is included for its allocator.  t_ofi_alloc [sym_top 0|1]: exit 0 when the whole sequence fits a 4096 MiB pool.

 * With a 2nd argument: the preallocated-slot pattern (the S31 fix), which must fit with sym-top 0 and 1 alike.
 * Sequence (MiB): slots 400 + 400 and a scratch 1023 (symmetric), staging 1023 + 1023 (taken and released per exchange, before the slots regrow), the slots regrown to
 * 822 each (free, then allocate), staging 1023 again.  First-fit everywhere (COMM_OFI_SYM_TOP=0) strands the staging; sym-top does not. */
#include "../comm_ofi.c"
#define MB ((size_t)1 << 20)
int main(int argc, char **argv)
{
    setenv("COMM_OFI_SYM_TOP", argc > 1 ? argv[1] : "1", 1);
    if (argc > 2) {   /* preallocated slots (S31 COMM_LAYER_VSLOT_MB_<g>): the same pool, the slots taken once at 822 MiB -> staging after the slots always fits */
        ofi_dev o2; memset(&o2, 0, sizeof o2); o2.bytes = 4096 * MB; pthread_mutex_init(&o2.lock, 0);
        o2.blocks = (struct oblk *)calloc(1, sizeof *o2.blocks); o2.blocks->len = o2.bytes;
        size_t t2 = comm_ofi_alloc(&o2, 1023 * MB, 1); (void)t2;
        size_t x0 = comm_ofi_alloc(&o2, 822 * MB, 1), x1 = comm_ofi_alloc(&o2, 822 * MB, 1); (void)x0; (void)x1;
        size_t q = comm_ofi_alloc(&o2, 1023 * MB, 0); comm_ofi_free(&o2, q);
        q = comm_ofi_alloc(&o2, 1023 * MB, 0); printf("t_ofi_alloc: prealloc: fits (in use %zu MiB, peak %zu MiB)\n", o2.cur / MB, o2.peak / MB); return 0;
    }
    ofi_dev od; memset(&od, 0, sizeof od); od.d = 0; od.bytes = 4096 * MB; pthread_mutex_init(&od.lock, 0);
    od.blocks = (struct oblk *)calloc(1, sizeof *od.blocks); od.blocks->off = 0; od.blocks->len = od.bytes;
    size_t s0 = comm_ofi_alloc(&od, 400 * MB, 1), s1 = comm_ofi_alloc(&od, 400 * MB, 1), tmp = comm_ofi_alloc(&od, 1023 * MB, 1);
    size_t a = comm_ofi_alloc(&od, 1023 * MB, 0), b = comm_ofi_alloc(&od, 1023 * MB, 0); comm_ofi_free(&od, a); comm_ofi_free(&od, b);
    comm_ofi_free(&od, s0); s0 = comm_ofi_alloc(&od, 822 * MB, 1);
    comm_ofi_free(&od, s1); s1 = comm_ofi_alloc(&od, 822 * MB, 1);
    a = comm_ofi_alloc(&od, 1023 * MB, 0); comm_ofi_free(&od, a);
    (void)tmp; printf("t_ofi_alloc: sym_top=%s: fits (in use %zu MiB, peak %zu MiB)\n", getenv("COMM_OFI_SYM_TOP"), od.cur / MB, od.peak / MB);
    return 0;
}
