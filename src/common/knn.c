/* knn.c — two-pass exact 5-NN per bucket.
 *
 * Pass 1 (coarse, bandwidth-light): sequential AVX2 int8 scan of the whole
 *   bucket, keeping the KNN_CAND nearest by approximate distance.
 * Pass 2 (exact): int16 squared distance over just those candidates, with the
 *   (distance, original-index) tie-break that reproduces main.c exactly.
 *
 * int8 halves memory traffic on the giant buckets (1M / 627k points) so the
 * scan stays predictable (no kd-tree high-dimensional tail). KNN_CAND is sized
 * so the true 5-NN is always inside the int8 shortlist; verify.c proves 0
 * disagreement vs full exact brute force and vs the main.c expected labels. */
#include "fraud.h"
#include <stdint.h>
#include <math.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

static inline int8_t q16_to_q8(int16_t v) {
    long r = lround((double)v * (double)SCALE8 / (double)SCALE);   /* v/10000*127 */
    if (r >  127) r =  127;
    if (r < -127) r = -127;
    return (int8_t)r;
}

/* approximate squared distance over the VDIM8 (=11) filter dims. q has lanes
 * 11..15 = 0; r is read 16-wide and its lanes 11..15 are masked to 0 so the
 * 5-byte over-read into the next vector never pollutes the distance. */
#if defined(__AVX2__)
static const int8_t DIST8_MASK[16] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0,0,0,0,0 };
#endif
static inline int32_t dist8(const int8_t *q, const int8_t *r) {
#if defined(__AVX2__)
    __m128i a = _mm_loadu_si128((const __m128i *)q);
    __m128i b = _mm_and_si128(_mm_loadu_si128((const __m128i *)r),
                              _mm_loadu_si128((const __m128i *)DIST8_MASK));
    __m256i d = _mm256_sub_epi16(_mm256_cvtepi8_epi16(a), _mm256_cvtepi8_epi16(b));
    __m256i m = _mm256_madd_epi16(d, d);
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(m), _mm256_extracti128_si256(m, 1));
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    return _mm_cvtsi128_si32(s);
#else
    int32_t s = 0;
    for (int k = 0; k < VDIM8; k++) { int e = q[k] - r[k]; s += e * e; }
    return s;
#endif
}

/* exact squared distance in int16 space */
static inline int64_t dist16(const int16_t *q, const int16_t *r) {
#if defined(__AVX2__)
    __m256i a = _mm256_loadu_si256((const __m256i *)q);
    __m256i b = _mm256_loadu_si256((const __m256i *)r);
    __m256i d = _mm256_sub_epi16(a, b);
    __m256i m = _mm256_madd_epi16(d, d);
    __m128i lo = _mm256_castsi256_si128(m), hi = _mm256_extracti128_si256(m, 1);
    __m128i s = _mm_add_epi32(lo, hi);
    __m128i s0 = _mm_cvtepi32_epi64(s);
    __m128i s1 = _mm_cvtepi32_epi64(_mm_srli_si128(s, 8));
    __m128i sum = _mm_add_epi64(s0, s1);
    return (int64_t)_mm_extract_epi64(sum, 0) + (int64_t)_mm_extract_epi64(sum, 1);
#else
    int64_t s = 0;
    for (int k = 0; k < VLANES; k++) { int32_t e = (int32_t)q[k] - r[k]; s += (int64_t)e * e; }
    return s;
#endif
}

static inline int is_fraud(const Dataset *ds, uint32_t i) {
    return (ds->fraud[i >> 3] >> (i & 7)) & 1u;
}

int knn_fraud_count(const Dataset *ds, const int16_t q16[VLANES], int bucket_key) {
    uint32_t lo = ds->hdr->bucket_off[bucket_key];
    uint32_t hi = ds->hdr->bucket_off[bucket_key + 1];
    if (lo >= hi) return 0;

    static const int dmap8[VDIM8] = DMAP8_INIT;
    int8_t q8[VLANES];
    for (int k = 0; k < VDIM8; k++) q8[k] = q16_to_q8(q16[dmap8[k]]);
    for (int k = VDIM8; k < VLANES; k++) q8[k] = 0;

    /* ── pass 1: int8 shortlist of KNN_CAND nearest (unsorted, max-tracked) ── */
    uint32_t n = hi - lo;
    int      cap = (n < KNN_CAND) ? (int)n : KNN_CAND;
    int32_t  cd[KNN_CAND];
    uint32_t ci[KNN_CAND];
    int      filled = 0, maxpos = 0;
    int32_t  maxd = INT32_MAX;
    const int8_t *b8 = ds->vecs8;
    for (uint32_t i = lo; i < hi; i++) {
        int32_t d = dist8(q8, b8 + (size_t)i * VDIM8);
        if (filled < cap) {
            cd[filled] = d; ci[filled] = i;
            if (filled == 0 || d > maxd) { maxd = d; maxpos = filled; }
            filled++;
            if (filled == cap) {                 /* recompute true max once full */
                maxd = cd[0]; maxpos = 0;
                for (int j = 1; j < cap; j++) if (cd[j] > maxd) { maxd = cd[j]; maxpos = j; }
            }
        } else if (d < maxd) {
            cd[maxpos] = d; ci[maxpos] = i;
            maxd = cd[0]; maxpos = 0;
            for (int j = 1; j < cap; j++) if (cd[j] > maxd) { maxd = cd[j]; maxpos = j; }
        }
    }

    /* ── pass 2: exact int16 re-rank → top-5 by (dist, original index) ── */
    int64_t  bd[KNN_K]; uint32_t bo[KNN_K]; uint32_t bi[KNN_K];
    for (int j = 0; j < KNN_K; j++) { bd[j] = INT64_MAX; bo[j] = UINT32_MAX; bi[j] = 0; }
    const int16_t *b16 = ds->vecs16;
    for (int c = 0; c < filled; c++) {
        uint32_t idx = ci[c];
        int64_t  d   = dist16(q16, b16 + (size_t)idx * VLANES);
        uint32_t oi  = ds->orig[idx];
        if (d > bd[KNN_K-1] || (d == bd[KNN_K-1] && oi >= bo[KNN_K-1])) continue;
        int j = KNN_K - 1;
        while (j > 0 && (d < bd[j-1] || (d == bd[j-1] && oi < bo[j-1]))) {
            bd[j] = bd[j-1]; bo[j] = bo[j-1]; bi[j] = bi[j-1]; j--;
        }
        bd[j] = d; bo[j] = oi; bi[j] = idx;
    }

    int frauds = 0;
    for (int j = 0; j < KNN_K; j++)
        if (bd[j] != INT64_MAX && is_fraud(ds, bi[j])) frauds++;
    return frauds;
}

/* Exact full brute force over all refs (verify only). */
int knn_fraud_count_full(const Dataset *ds, const int16_t q16[VLANES]) {
    int64_t  bd[KNN_K]; uint32_t bo[KNN_K]; uint32_t bi[KNN_K];
    for (int j = 0; j < KNN_K; j++) { bd[j] = INT64_MAX; bo[j] = UINT32_MAX; bi[j] = 0; }
    const int16_t *b16 = ds->vecs16;
    for (uint32_t i = 0; i < ds->nrefs; i++) {
        int64_t  d  = dist16(q16, b16 + (size_t)i * VLANES);
        uint32_t oi = ds->orig[i];
        if (d > bd[KNN_K-1] || (d == bd[KNN_K-1] && oi >= bo[KNN_K-1])) continue;
        int j = KNN_K - 1;
        while (j > 0 && (d < bd[j-1] || (d == bd[j-1] && oi < bo[j-1]))) {
            bd[j] = bd[j-1]; bo[j] = bo[j-1]; bi[j] = bi[j-1]; j--;
        }
        bd[j] = d; bo[j] = oi; bi[j] = i;
    }
    int frauds = 0;
    for (int j = 0; j < KNN_K; j++)
        if (bd[j] != INT64_MAX && is_fraud(ds, bi[j])) frauds++;
    return frauds;
}
