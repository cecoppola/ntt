/* hosttl.h - ECALC_HOST_TL=1: host timeline of one PE at the exchange layer (header only; used by comm_xshmem.c).
 * Wall time from the end of the last pending exchange to the next post is attributed to the XK_* tag of that post
 * ("host", s), including kernel launches and stream syncs between exchanges. Time the post itself waits for the
 * caller's stream (hipStreamSynchronize before the publish) is "wait". A post made while another exchange is pending
 * has no gap ("ovl"; npend counts exchanges of all threads and communicators of the PE, so with several posting
 * threads the gap goes to whichever posts first after an idle period). busy = time with at least one exchange
 * pending, so span (first post to last end) = busy + sum of the host column; "tail" = last end to the report. Only
 * clock reads and a short per-PE lock; no GPU call, no sync, no allocation. */
#ifndef HOSTTL_H
#define HOSTTL_H
#include <pthread.h>
#include <stdio.h>
#define HTL_NK 8
typedef struct {
    int on, npend; pthread_mutex_t m;
    double first, last_end, bstart, busy, host[HTL_NK], wait[HTL_NK], hmax[HTL_NK], wmax[HTL_NK]; long cnt[HTL_NK], ovl[HTL_NK];
} htl_t;
static void htl_init(htl_t *h, int on) { *h = (htl_t){0}; h->on = on; pthread_mutex_init(&h->m, 0); }
static void htl_post(htl_t *h, int tag, double now)
{
    if (!h->on) return;
    if (tag < 0 || tag >= HTL_NK) tag = 0;
    pthread_mutex_lock(&h->m);
    if (h->npend > 0) h->ovl[tag]++;
    else {
        if (h->first == 0) h->first = now;
        else { double g = now - h->last_end; h->host[tag] += g; h->cnt[tag]++; if (g > h->hmax[tag]) h->hmax[tag] = g; }
        h->bstart = now;
    }
    h->npend++;
    pthread_mutex_unlock(&h->m);
}
static void htl_wait(htl_t *h, int tag, double dt)
{
    if (!h->on) return;
    if (tag < 0 || tag >= HTL_NK) tag = 0;
    pthread_mutex_lock(&h->m); h->wait[tag] += dt; if (dt > h->wmax[tag]) h->wmax[tag] = dt; pthread_mutex_unlock(&h->m);
}
static void htl_end(htl_t *h, double now)
{
    if (!h->on) return;
    pthread_mutex_lock(&h->m);
    if (h->npend > 0 && --h->npend == 0) { h->last_end = now; h->busy += now - h->bstart; }
    pthread_mutex_unlock(&h->m);
}
/* the table: one row per tag with a post (gaps = posts that followed an idle PE; the first post has no gap) */
static void htl_print(FILE *f, htl_t *h, const char *const *names, int nk, int rank, double now)
{
    double sh = 0, sw = 0; long sc = 0, so = 0;
    fprintf(f, "hosttl pe %d: tag gaps host_s wait_s host_max_s wait_max_s ovl\n", rank);
    for (int k = 0; k < nk && k < HTL_NK; k++) {
        if (!h->cnt[k] && !h->ovl[k] && h->wait[k] == 0) continue;
        fprintf(f, "hosttl pe %d: %-6s %8ld %10.3f %10.3f %10.4f %10.4f %6ld\n", rank, names[k], h->cnt[k], h->host[k], h->wait[k], h->hmax[k], h->wmax[k], h->ovl[k]);
        sh += h->host[k]; sw += h->wait[k]; sc += h->cnt[k]; so += h->ovl[k]; }
    double span = h->first > 0 ? h->last_end - h->first : 0;
    fprintf(f, "hosttl pe %d: TOTAL  %8ld %10.3f %10.3f %27s %6ld\n", rank, sc, sh, sw, "", so);
    fprintf(f, "hosttl pe %d: span first post to last end %.3f s = busy (an exchange pending) %.3f + host %.3f (check %.6f); wait %.3f s lies inside busy; tail (last end to report) %.3f s\n",
            rank, span, h->busy, sh, span - h->busy - sh, sw, h->last_end > 0 ? now - h->last_end : 0.0);
}
#endif
