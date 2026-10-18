/* t_trace.c - the COMM_TRACE writer of comm_trace.h without SHMEM: 4 threads stamp records through tr_put (with a chunk-claim race and an arena that fills),
 * then tr_write_file writes the file.  Usage: COMM_TRACE=1 COMM_TRACE_MAX_MB=16 tests/t_trace OUTDIR   (prints "records N dropped D"; exits 1 on a wrong count).
 * tools/trace_report.py reads the file back (TESTING.md). */
#include "../comm_trace.h"
#include <unistd.h>
#define NT 4
#define PER 400000
static void *th(void *a)
{
    long id = (long)a; tr_buf b = {0, 0};
    for (uint32_t i = 0; i < PER; i++) tr_put(&b, 1 + (uint8_t)(i % 5), 0, 7, i, i, (uint32_t)id, 0, (int)id, 1, 1000 + i);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    tr_init(0);
    if (!g_tr.on) { fprintf(stderr, "t_trace: set COMM_TRACE=1\n"); return 2; }
    pthread_t t[NT]; for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, th, (void *)i); for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
    int node[2] = {0, 1}; tr_comm_add(7, 2, 0, 0, node);
    char js[512]; FILE *f = fmemopen(js, sizeof js, "w"); fprintf(f, "{\"version\":1,\"proxy\":0,\"node\":0,\"nodes\":1,\"events\":"); tr_json_events(f); fprintf(f, ",\"comms\":[]}"); long jl = ftell(f); fclose(f);
    char path[512]; snprintf(path, sizeof path, "%s/trace_pe0.bin", argv[1]);
    uint64_t want = tr_count(); long w = tr_write_file(path, js, (size_t)jl);
    printf("records %ld dropped %llu arena_records %llu\n", w, (unsigned long long)g_tr.dropped, (unsigned long long)want);
    uint64_t cap = (uint64_t)g_tr.nchunks * TR_CHUNK_RECS, tot = (uint64_t)NT * PER;
    /* every record is either in the file or counted as dropped (a thread dropping stops at its own full chunk: dropped >= tot - cap) */
    if (w < 0 || (uint64_t)w != want || (uint64_t)w > cap || (uint64_t)w + g_tr.dropped < tot) { printf("FAIL\n"); return 1; }
    printf("PASS\n"); return 0;
}
