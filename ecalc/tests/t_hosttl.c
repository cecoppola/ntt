/* t_hosttl - the host timeline bookkeeping of hosttl.h (ECALC_HOST_TL), host only, injected times. */
#include "harness.h"
#include "../hosttl.h"
#include <math.h>
static const char *const nm[3] = { "other", "layeq", "addsh" };
int main(void)
{
    htl_t h; htl_init(&h, 1);
    htl_post(&h, 1, 10.0); htl_wait(&h, 1, 0.5); htl_end(&h, 12.0);     /* first post: no gap */
    htl_post(&h, 1, 15.0); htl_wait(&h, 1, 1.0); htl_end(&h, 16.0);     /* gap 3 to layeq */
    htl_post(&h, 2, 16.5); htl_post(&h, 1, 17.0); htl_end(&h, 17.5); htl_end(&h, 18.0);   /* gap .5 to addsh; second post overlaps */
    htl_post(&h, 9, 20.0); htl_end(&h, 21.0);                           /* a tag out of range counts as 0 (gap 2) */
    VERIFY(h.cnt[1] == 1 && fabs(h.host[1] - 3.0) < 1e-12, "layeq gap");
    VERIFY(h.cnt[2] == 1 && fabs(h.host[2] - 0.5) < 1e-12, "addsh gap");
    VERIFY(h.cnt[0] == 1 && fabs(h.host[0] - 2.0) < 1e-12, "bad tag -> other");
    VERIFY(h.ovl[1] == 1 && h.ovl[2] == 0, "overlap counted, no gap");
    VERIFY(fabs(h.wait[1] - 1.5) < 1e-12 && fabs(h.wmax[1] - 1.0) < 1e-12 && fabs(h.hmax[1] - 3.0) < 1e-12, "wait and maxima");
    double span = h.last_end - h.first; VERIFY(fabs(span - h.busy - (h.host[0] + h.host[1] + h.host[2])) < 1e-12, "span = busy + host");
    VERIFY(h.npend == 0, "all ended");
    htl_t o; htl_init(&o, 0); htl_post(&o, 1, 1.0); htl_wait(&o, 1, 1.0); htl_end(&o, 2.0);
    VERIFY(o.first == 0 && o.busy == 0 && o.cnt[1] == 0 && o.wait[1] == 0, "off: nothing recorded");
    char *buf = 0; size_t bl = 0; FILE *f = open_memstream(&buf, &bl); htl_print(f, &h, nm, 3, 7, 22.0); fclose(f);
    VERIFY(strstr(buf, "hosttl pe 7: layeq") && strstr(buf, "TOTAL") && strstr(buf, "check 0.000000") && strstr(buf, "tail (last end to report) 1.000"), "table");
    free(buf);
    return verify_done("t_hosttl");
}
