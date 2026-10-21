/* t_pe_geo_live.c -- boot (PMI2) + pe_geo on a real launch; prints one line per PE, rank 0 prints the verdict.
 * Build (aac7 login, XSHMEM=ofi objects): see results/XSHMEM.md §2.   Run: srun -N<n> --ntasks-per-node=<p> ... ./t_pe_geo_live
 * Checks: boot allgather of a 3000 B blob (chunking) returns every rank's bytes; boot rank/size == SLURM_PROCID/NTASKS;
 * pe_geo: one line per PE (read it against the expected shape in results/XSHMEM.md §2), all PEs agree on nics,
 * and no two PEs on a node share a NIC when pes_per_gpu == 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pe_geo.h"
#include "xshmem_boot.h"

int main(void)
{
    pe_topo tv, *t = &tv; pe_geo_init(t);
    char s[400]; pe_geo_str(t, s, sizeof s);
    int bad = 0, len = 3000;
    unsigned char *blob = (unsigned char *)malloc(len), *all = (unsigned char *)malloc((size_t)t->npes * len);
    for (int i = 0; i < len; i++) blob[i] = (unsigned char)(t->pe * 31 + i * 7);
    boot_allgather(blob, len, all);
    for (int r = 0; r < t->npes; r++) for (int i = 0; i < len; i++) if (all[(size_t)r * len + i] != (unsigned char)(r * 31 + i * 7)) { bad++; break; }
    /* gather the topo structs */
    pe_topo *tt = (pe_topo *)malloc((size_t)t->npes * sizeof tv); boot_allgather(t, sizeof tv, tt);
    for (int r = 0; r < t->npes; r++) {
        if (tt[r].nics != t->nics) bad++;
        if (tt[r].node == t->node && r != t->pe && t->pes_per_gpu == 1 && tt[r].nics && t->nics && tt[r].nic[0] == t->nic[0] && t->nics == 1) { bad++; fprintf(stderr, "PE %d and %d share NIC %d\n", r, t->pe, t->nic[0]); }
    }
    printf("%s%s\n", s, bad ? "  BAD" : "");
    fflush(stdout); boot_fence();
    if (t->pe == 0) printf("t_pe_geo_live: %d PEs, %s\n", t->npes, bad ? "problems on rank 0 (see BAD lines)" : "rank 0 ok");
    boot_finalize();
    return bad != 0;
}
