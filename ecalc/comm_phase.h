/* comm_phase.h - phase tags (COMM_TRACE_PH, COMM_TRACE_WD_MS; default off): a per-thread "what is this thread doing" word, set at entry and exit of the calls
 * and steps a node-wide stall could sit in (hip allocation / registration / stream syncs, the v-slot alloc and release, the exchange post and wait by kind,
 * the redistribution / transform / CRT / result / carry steps of rns_dist.c, the setup before the first exchange).  Use:  int ph_ = ec_ph(EC_PH_X); ...; ec_ph(ph_);
 * (ec_ph returns the previous tag, so nesting restores).  Off: one load and branch.  On: comm_trace.h (tr_ph_set) records the word per thread and, with
 * COMM_TRACE_PH=1, an H_PHASE event per change; the watchdog and heartbeat snapshots print every thread's word.  Changes no digit. */
#ifndef EC_COMM_PHASE_H
#define EC_COMM_PHASE_H
#define EC_PH_LIST(X) \
    X(NONE) X(MN_INIT) X(SHMEM_INIT) X(POOL_REG) X(PX_ATTACH) X(MESH_CREATE) X(LAYER_CREATE) X(PLAN_CREATE) \
    X(HIP_MALLOC) X(HIP_FREE) X(HIP_HOSTREG) X(HIP_SYNC) X(HIP_COPY) X(SYM_ALLOC) X(SYM_FREE) X(VSLOT_ALLOC) X(VSLOT_RELEASE) X(VTMP_ALLOC) X(DPOOL) \
    X(POST_OTHER) X(POST_REDIST) X(POST_RESULT) X(POST_SPILL) X(POST_ADDSH) X(POST_LAYEQ) X(POST_LAYV) \
    X(WAIT_OTHER) X(WAIT_REDIST) X(WAIT_RESULT) X(WAIT_SPILL) X(WAIT_ADDSH) X(WAIT_LAYEQ) X(WAIT_LAYV) \
    X(RD_REDIST) X(RD_FWD) X(RD_CRT) X(RD_RESULT) X(RD_CARRY) X(ADDSH_STEP)
enum {
#define X(n) EC_PH_##n,
    EC_PH_LIST(X)
#undef X
    EC_PH_N
};
#define EC_PH_POST(k) (EC_PH_POST_OTHER + (k))       /* k = the exchange kind (XK_*, comm.h) */
#define EC_PH_WAIT(k) (EC_PH_WAIT_OTHER + (k))
#ifdef __cplusplus
extern "C" {
#endif
extern int ec_ph_on;                                  /* defined in comm_util.c; set by comm_trace.h when phase tags are on */
extern int (*ec_ph_fn)(int);
static inline int ec_ph(int id) { return ec_ph_on ? ec_ph_fn(id) : 0; }
#ifdef __cplusplus
}
#endif
#define HIP_SYNC(s) do { int ph_ = ec_ph(EC_PH_HIP_SYNC); HIP_CHECK(hipStreamSynchronize(s)); ec_ph(ph_); } while (0)   /* where HIP_CHECK exists */
#endif
