/* crt.c - see crt.h.  From bench/20_cpu (garner4, crt_carry_par_tight). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "crt.h"
#include "modarith.h"
#include "mem.h"
#include "bigint.h"

typedef unsigned __int128 u128;
#define PR ec_P

static inline uint64_t mod3(const uint64_t *x, uint64_t p)
{
    u128 t = x[2] % p;
    t = ((t << 64) | x[1]) % p;
    t = ((t << 64) | x[0]) % p;
    return (uint64_t)t;
}
static struct garner { uint64_t c1, c2, c3; uint64_t M1[2], M2[3]; int ready; } G;

void crt_init(void)
{
    if (G.ready) return;
    u128 m1 = (u128)PR[0] * PR[1];
    G.c1 = ec_inv(PR[0] % PR[1], PR[1]);
    G.M1[0] = (uint64_t)m1; G.M1[1] = (uint64_t)(m1 >> 64);
    G.c2 = ec_inv((uint64_t)(m1 % PR[2]), PR[2]);
    { u128 lo = (u128)G.M1[0] * PR[2], hi = (u128)G.M1[1] * PR[2] + (lo >> 64);
      G.M2[0] = (uint64_t)lo; G.M2[1] = (uint64_t)hi; G.M2[2] = (uint64_t)(hi >> 64); }
    G.c3 = ec_inv(mod3(G.M2, PR[3]), PR[3]);
    G.ready = 1;
}

static inline void garner4(const uint64_t r[4], uint64_t out[4])
{
    uint64_t x[4] = { r[0], 0, 0, 0 };
    u128 t;
    { uint64_t xm = x[0] % PR[1]; uint64_t t1 = (uint64_t)((u128)((r[1] + PR[1] - xm) % PR[1]) * G.c1 % PR[1]);
      t = (u128)t1 * PR[0] + x[0]; x[0] = (uint64_t)t; x[1] = (uint64_t)(t >> 64); }
    { uint64_t xm = (uint64_t)((((u128)x[1] % PR[2]) << 64 | x[0]) % PR[2]);
      uint64_t t2 = (uint64_t)((u128)((r[2] + PR[2] - xm) % PR[2]) * G.c2 % PR[2]);
      u128 lo = (u128)t2 * G.M1[0] + x[0], hi = (u128)t2 * G.M1[1] + x[1] + (lo >> 64);
      x[0] = (uint64_t)lo; x[1] = (uint64_t)hi; x[2] = (uint64_t)(hi >> 64); }
    { uint64_t xm = mod3(x, PR[3]);
      uint64_t t3 = (uint64_t)((u128)((r[3] + PR[3] - xm) % PR[3]) * G.c3 % PR[3]);
      u128 s0 = (u128)t3 * G.M2[0] + x[0];
      u128 s1 = (u128)t3 * G.M2[1] + x[1] + (s0 >> 64);
      u128 s2 = (u128)t3 * G.M2[2] + x[2] + (s1 >> 64);
      out[0] = (uint64_t)s0; out[1] = (uint64_t)s1; out[2] = (uint64_t)s2; out[3] = (uint64_t)(s2 >> 64); }
}
void crt_garner4(const uint64_t r[4], uint64_t out[4]) { crt_init(); garner4(r, out); }

void crt_carry_par4(uint64_t *const res[4], size_t n, uint64_t *out, int T)
{
    crt_init();
    for (size_t i = n; i < n + 4; i++) out[i] = 0;
    if (T < 1) T = 1;
    if ((size_t)T > n / 64 + 1) T = (int)(n / 64 + 1);
    uint64_t (*spill)[4] = (uint64_t (*)[4])calloc(T, sizeof *spill);
    int t;
#pragma omp parallel for num_threads(T) schedule(static)
    for (t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T, k;
        uint64_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        for (k = k0; k < k1; k++) {
            uint64_t r[4] = { res[0][k], res[1][k], res[2][k], res[3][k] }, c[4];
            garner4(r, c);
            if (bi_decimal) {                            /* four base-B digits; window of 4 digits + carry */
                uint64_t d[4]; ec_words_to_dec4(c, d);
                uint64_t s = w0 + d[0], cy = s >= BI_B10; out[k] = cy ? s - BI_B10 : s;
                s = w1 + d[1] + cy; cy = s >= BI_B10; w0 = cy ? s - BI_B10 : s;
                s = w2 + d[2] + cy; cy = s >= BI_B10; w1 = cy ? s - BI_B10 : s;
                s = w3 + d[3] + cy; cy = s >= BI_B10; w2 = cy ? s - BI_B10 : s;
                w3 = cy;
            } else {
                u128 s;
                s = (u128)w0 + c[0]; out[k] = (uint64_t)s;
                s = (s >> 64) + w1 + c[1]; w0 = (uint64_t)s;
                s = (s >> 64) + w2 + c[2]; w1 = (uint64_t)s;
                w2 = (uint64_t)(s >> 64) + c[3];
            }
        }
        spill[t][0] = w0; spill[t][1] = w1; spill[t][2] = w2; spill[t][3] = w3;
    }
    for (t = 0; t < T; t++) {
        size_t k1 = n * (t + 1) / T, i;
        if (bi_decimal) {
            uint64_t cy = 0;
            for (i = 0; i < 4 && k1 + i < n + 4; i++) { uint64_t s = out[k1 + i] + spill[t][i] + cy; cy = s >= BI_B10; out[k1 + i] = cy ? s - BI_B10 : s; }
            for (; cy && k1 + i < n + 4; i++) { uint64_t s = out[k1 + i] + cy; cy = s >= BI_B10; out[k1 + i] = cy ? s - BI_B10 : s; }
        } else {
            u128 s = 0;
            for (i = 0; i < 3; i++) { s += (u128)out[k1 + i] + spill[t][i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
            for (i = 3; s && k1 + i < n + 4; i++) { s += out[k1 + i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
        }
    }
    free(spill);
}

void crt_carry_par4_q(uint64_t *const buf[4], size_t Q, size_t n, uint64_t *out, int T)
{
    crt_init();
    if (bi_decimal) { fprintf(stderr, "crt_carry_par4_q: binary base only\n"); abort(); }
    for (size_t i = n; i < n + 4; i++) out[i] = 0;
    if (T < 4) T = 4;
    T = T / 4 * 4;
    int per = T / 4;
    uint64_t (*spill)[4] = (uint64_t (*)[4])calloc(T, sizeof *spill);
    size_t *k1s = (size_t *)calloc(T, sizeof *k1s);
    int t;
#pragma omp parallel for num_threads(T) schedule(static)
    for (t = 0; t < T; t++) {
        int q = t / per, j = t % per;
        size_t q0 = q * Q, q1 = (q + 1) * Q < n ? (q + 1) * Q : n;
        if (q0 > n) q0 = n;
        size_t len = q1 - q0, k0 = q0 + len * j / per, k1 = q0 + len * (j + 1) / per, k;
        const uint64_t *r0 = buf[q] - q0, *r1 = buf[q] + Q - q0, *r2 = buf[q] + 2 * Q - q0, *r3 = buf[q] + 3 * Q - q0;
        mem_pin_to_node(q);
        uint64_t w0 = 0, w1 = 0, w2 = 0;
        for (k = k0; k < k1; k++) {
            uint64_t r[4] = { r0[k], r1[k], r2[k], r3[k] }, c[4];
            u128 s;
            garner4(r, c);
            s = (u128)w0 + c[0]; out[k] = (uint64_t)s;
            s = (s >> 64) + w1 + c[1]; w0 = (uint64_t)s;
            s = (s >> 64) + w2 + c[2]; w1 = (uint64_t)s;
            w2 = (uint64_t)(s >> 64) + c[3];
        }
        spill[t][0] = w0; spill[t][1] = w1; spill[t][2] = w2; k1s[t] = k1;
        mem_unpin();
    }
    for (t = 0; t < T; t++) {
        size_t k1 = k1s[t], i; u128 s = 0;
        for (i = 0; i < 3; i++) { s += (u128)out[k1 + i] + spill[t][i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
        for (i = 3; s && k1 + i < n + 4; i++) { s += out[k1 + i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
    }
    free(spill); free(k1s);
}
