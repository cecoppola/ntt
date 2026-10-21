/* xshmem_boot.c -- bootstrap for the xshmem libfabric backend (XSHMEM=ofi only; see xshmem_boot.h).
 * BOOT=pmi2 (default): Cray PMI2 key/value space, KVS values hex-encoded and chunked at 900 characters.
 * BOOT=pmix: build option (-DBOOT_PMIX), a stub in P3 that aborts with a message. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xshmem_boot.h"

#ifndef BOOT_PMIX
#include <pmi2.h>
#endif
#ifndef BOOT_PMIX
static int boot_up_flag(void);
#endif
void boot_abort(const char *msg)
{
#ifndef BOOT_PMIX
    if (boot_up_flag()) PMI2_Abort(1, msg ? msg : "xshmem abort");   /* kills the whole job step: peers must not hang in a fence */
#endif
    abort();
}
static void bdie(const char *m, int rc) { fprintf(stderr, "xshmem_boot: %s (rc %d)\n", m, rc); fflush(stderr); boot_abort(m); }

#ifdef BOOT_PMIX
int  boot_init(int *rank, int *size) { (void)rank; (void)size; bdie("BOOT=pmix is a stub in P3 (use BOOT=pmi2)", 0); return -1; }
int  boot_allgather(const void *b, size_t l, void *o) { (void)b; (void)l; (void)o; bdie("BOOT=pmix stub", 0); return -1; }
void boot_fence(void) { bdie("BOOT=pmix stub", 0); }
void boot_finalize(void) {}
#else
#include <pmi2.h>
#ifndef PMI2_ID_NULL
#define PMI2_ID_NULL (-1)
#endif
#define CHUNK_CH 900                       /* characters per KVS value; 2 per raw byte */
#define CHUNK_B  (CHUNK_CH / 2)

static struct { int up, rank, size, seq; char job[PMI2_MAX_VALLEN]; } B;
static int boot_up_flag(void) { return B.up; }

static int env_int(const char *n, int def) { const char *e = getenv(n); return (e && *e) ? atoi(e) : def; }

int boot_init(int *rank, int *size)
{
    if (!B.up) {
        /* Cray PMI2 keeps the KVS in a fixed shared segment (default far too small: 30 entries aborted 8 PEs).  Every allgather puts
         * ceil(len/450) keys per PE and keys are never freed (about 5 allgathers per job), so size it from the task count: 16 per task + 256. */
        if (!getenv("PMI_MAX_KVS_ENTRIES")) { char v[32]; int nt = env_int("SLURM_NTASKS", 64); snprintf(v, sizeof v, "%d", 16 * nt + 256); setenv("PMI_MAX_KVS_ENTRIES", v, 0); }
        int spawned = 0, appnum = 0, rc = PMI2_Init(&spawned, &B.size, &B.rank, &appnum);
        if (rc != PMI2_SUCCESS) bdie("PMI2_Init failed (run under plain srun, no --mpi)", rc);
        rc = PMI2_Job_GetId(B.job, sizeof B.job);
        if (rc != PMI2_SUCCESS) bdie("PMI2_Job_GetId failed", rc);
        int sr = env_int("SLURM_PROCID", -1), sn = env_int("SLURM_NTASKS", -1);
        if ((sr >= 0 && sr != B.rank) || (sn >= 0 && sn != B.size)) {
            fprintf(stderr, "xshmem_boot: PMI2 rank/size %d/%d != SLURM_PROCID/SLURM_NTASKS %d/%d\n", B.rank, B.size, sr, sn);
            bdie("rank/size mismatch", 0);
        }
        B.up = 1;
    }
    if (rank) *rank = B.rank;
    if (size) *size = B.size;
    return 0;
}

void boot_fence(void)
{
    if (!B.up) bdie("boot_fence before boot_init", 0);
    int rc = PMI2_KVS_Fence(); if (rc != PMI2_SUCCESS) bdie("PMI2_KVS_Fence failed", rc);
}

int boot_allgather(const void *blob, size_t len, void *out)
{
    static const char hx[] = "0123456789abcdef";
    if (!B.up) bdie("boot_allgather before boot_init", 0);
    const unsigned char *p = (const unsigned char *)blob; unsigned char *o = (unsigned char *)out;
    size_t nch = (len + CHUNK_B - 1) / CHUNK_B; int seq = B.seq++;
    char key[64], val[CHUNK_CH + 1];
    for (size_t c = 0; c < nch; c++) {
        size_t n = len - c * CHUNK_B; if (n > CHUNK_B) n = CHUNK_B;
        for (size_t i = 0; i < n; i++) { unsigned char v = p[c * CHUNK_B + i]; val[2 * i] = hx[v >> 4]; val[2 * i + 1] = hx[v & 15]; }
        val[2 * n] = 0;
        snprintf(key, sizeof key, "xb%d_%d_%zu", seq, B.rank, c);
        int rc = PMI2_KVS_Put(key, val); if (rc != PMI2_SUCCESS) bdie("PMI2_KVS_Put failed", rc);
    }
    boot_fence();
    for (int r = 0; r < B.size; r++)
        for (size_t c = 0; c < nch; c++) {
            size_t n = len - c * CHUNK_B; if (n > CHUNK_B) n = CHUNK_B;
            int vl = 0; snprintf(key, sizeof key, "xb%d_%d_%zu", seq, r, c);
            int rc = PMI2_KVS_Get(B.job, PMI2_ID_NULL, key, val, (int)sizeof val, &vl);
            if (rc != PMI2_SUCCESS) bdie("PMI2_KVS_Get failed", rc);
            if ((size_t)vl != 2 * n) { fprintf(stderr, "xshmem_boot: chunk %s has %d chars, want %zu\n", key, vl, 2 * n); bdie("bad chunk length", 0); }
            for (size_t i = 0; i < n; i++) {
                int a = val[2 * i], b = val[2 * i + 1];
                a = a <= '9' ? a - '0' : a - 'a' + 10; b = b <= '9' ? b - '0' : b - 'a' + 10;
                o[(size_t)r * len + c * CHUNK_B + i] = (unsigned char)(a * 16 + b);
            }
        }
    boot_fence();   /* keys are not reused, but a rank must not finalize while others still read */
    return 0;
}

void boot_finalize(void) { if (B.up) { PMI2_Finalize(); B.up = 0; } }
#endif
