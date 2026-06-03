/* knn.c — per-bucket IVF 5-NN: centroid probe + vpmaddwd int8 scan + int16 re-rank.
 *
 * The bucket is partitioned (offline k-means) into clusters stored contiguously.
 * A query scores the bucket's cluster centroids, probes the NPROBE nearest, and
 * runs the coarse int8 scan only over those clusters. The int8 data is dim-pair
 * interleaved SoA per cluster: for 8 points we load, per dim-pair, 16 int8, sub
 * the broadcast query pair, and vpmaddwd(diff,diff) yields 8 int32 = (Δa²+Δb²) —
 * no per-point horizontal sum. We keep the KNN_CAND nearest by this coarse int8
 * distance across all probed clusters, then re-rank them exactly in int16 with
 * the (distance, original index) tie-break that reproduces the reference labels. */
#include "fraud.h"
#include <stdint.h>
#include <math.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

static inline int8_t q16_to_q8(int16_t v) {
    long r = lround((double)v * (double)SCALE8 / (double)SCALE);
    if (r >  127) r =  127;
    if (r < -127) r = -127;
    return (int8_t)r;
}

static inline int64_t dist16v(const int16_t *q, const int16_t *r) {
#if defined(__AVX2__)
    __m256i a = _mm256_loadu_si256((const __m256i *)q);
    __m256i b = _mm256_and_si256(_mm256_loadu_si256((const __m256i *)r),
                                 _mm256_setr_epi16(-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0,0,0,0,0));
    __m256i d = _mm256_sub_epi16(a, b);
    __m256i m = _mm256_madd_epi16(d, d);
    __m128i lo = _mm256_castsi256_si128(m), hi = _mm256_extracti128_si256(m, 1);
    __m128i s = _mm_add_epi32(lo, hi);
    __m128i s0 = _mm_cvtepi32_epi64(s);
    __m128i s1 = _mm_cvtepi32_epi64(_mm_srli_si128(s, 8));
    __m128i sum = _mm_add_epi64(s0, s1);
    return (int64_t)_mm_extract_epi64(sum,0)+(int64_t)_mm_extract_epi64(sum,1);
#else
    int64_t s=0; for(int k=0;k<VDIM8;k++){int32_t e=(int32_t)q[k]-r[k];s+=(int64_t)e*e;} return s;
#endif
}

static inline int is_fraud(const Dataset *ds, uint32_t i){ return (ds->fraud[i>>3]>>(i&7))&1u; }

static inline void top_insert(int64_t d, uint32_t oi, uint32_t idx,
                              int64_t *bd, uint32_t *bo, uint32_t *bi) {
    if (d > bd[KNN_K-1] || (d == bd[KNN_K-1] && oi >= bo[KNN_K-1])) return;
    int j = KNN_K-1;
    while (j>0 && (d<bd[j-1] || (d==bd[j-1] && oi<bo[j-1]))) { bd[j]=bd[j-1];bo[j]=bo[j-1];bi[j]=bi[j-1];j--; }
    bd[j]=d; bo[j]=oi; bi[j]=idx;
}

typedef struct { int32_t cd[KNN_CAND]; uint32_t ci[KNN_CAND]; int filled, maxpos, cap; int32_t maxd; } Cand;
static inline void cand_offer(Cand *C, int32_t d, uint32_t idx) {
    if (C->filled < C->cap) {
        C->cd[C->filled]=d; C->ci[C->filled]=idx;
        if (C->filled==0 || d>C->maxd){ C->maxd=d; C->maxpos=C->filled; }
        C->filled++;
        if (C->filled==C->cap){ C->maxd=C->cd[0]; C->maxpos=0; for(int j=1;j<C->cap;j++) if(C->cd[j]>C->maxd){C->maxd=C->cd[j];C->maxpos=j;} }
    } else if (d < C->maxd) {
        C->cd[C->maxpos]=d; C->ci[C->maxpos]=idx;
        C->maxd=C->cd[0]; C->maxpos=0; for(int j=1;j<C->cap;j++) if(C->cd[j]>C->maxd){C->maxd=C->cd[j];C->maxpos=j;}
    }
}

/* Coarse int8 scan of one cluster's points [lo, lo+cnt) into the shortlist C. */
static inline void scan_cluster(const Dataset *ds, const int8_t *q8, uint32_t lo, uint32_t cnt, Cand *C) {
    const int8_t *soa = ds->vecs8 + (size_t)lo * VPAD;
    uint32_t p = 0;
#if defined(__AVX2__)
    __m256i qp[GPAIRS];
    for (int pp=0; pp<GPAIRS; pp++)
        qp[pp] = _mm256_set1_epi32((int)((uint16_t)q8[2*pp] | ((uint32_t)(uint16_t)q8[2*pp+1] << 16)));
    for (; p + 8 <= cnt; p += 8) {
        __m256i acc = _mm256_setzero_si256();
        for (int pp=0; pp<GPAIRS; pp++) {
            __m128i r8  = _mm_loadu_si128((const __m128i *)(soa + (size_t)pp*2*cnt + (size_t)p*2));
            __m256i r16 = _mm256_cvtepi8_epi16(r8);
            __m256i df  = _mm256_sub_epi16(r16, qp[pp]);
            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(df, df));
        }
        if (C->filled == C->cap) {
            __m256i mx = _mm256_set1_epi32(C->maxd);
            __m256i lt = _mm256_cmpgt_epi32(mx, acc);     /* acc < maxd -> -1 */
            if (_mm256_testz_si256(lt, lt)) continue;
        }
        int32_t dist[8];
        _mm256_storeu_si256((__m256i *)dist, acc);
        for (int k=0;k<8;k++) cand_offer(C, dist[k], lo + p + (uint32_t)k);
    }
#endif
    for (; p < cnt; p++) {
        int32_t s=0;
        for (int pp=0; pp<GPAIRS; pp++) {
            int ea = q8[2*pp]   - soa[(size_t)pp*2*cnt + (size_t)p*2 + 0];
            int eb = q8[2*pp+1] - soa[(size_t)pp*2*cnt + (size_t)p*2 + 1];
            s += ea*ea + eb*eb;
        }
        cand_offer(C, s, lo + p);
    }
}

int knn_fraud_count(const Dataset *ds, const int16_t q16[VLANES], int bucket_key, int nprobe) {
    uint32_t cb0 = ds->hdr->clust_bucket_off[bucket_key];
    uint32_t cb1 = ds->hdr->clust_bucket_off[bucket_key+1];
    uint32_t ncl = cb1 - cb0;
    if (ncl == 0) return 0;

    static const int dmap8[VDIM8] = DMAP8_INIT;
    int16_t qv[VLANES]; int8_t q8[VPAD];
    for (int k=0;k<VDIM8;k++){ qv[k]=q16[dmap8[k]]; q8[k]=q16_to_q8(qv[k]); }
    for (int k=VDIM8;k<VLANES;k++) qv[k]=0;
    for (int k=VDIM8;k<VPAD;k++)   q8[k]=0;

    Cand C; C.filled=0; C.maxpos=0; C.maxd=INT32_MAX; C.cap=KNN_CAND;

    if (nprobe <= 0 || nprobe >= (int)ncl) {
        /* probe every cluster — exact within the bucket */
        for (uint32_t c=cb0; c<cb1; c++) {
            uint32_t lo = ds->clust_pt_off[c], hi = ds->clust_pt_off[c+1];
            scan_cluster(ds, q8, lo, hi-lo, &C);
        }
    } else {
        /* select the nprobe nearest centroids (sorted-insert top-P) */
        int P = nprobe; if (P > NPROBE_MAX) P = NPROBE_MAX;
        int64_t pd[NPROBE_MAX]; uint32_t pc[NPROBE_MAX];
        int filled = 0;
        const int16_t *cent = ds->centroids;
        for (uint32_t c=cb0; c<cb1; c++) {
            int64_t d = dist16v(qv, cent + (size_t)c*VLANES);
            if (filled < P) {
                int j = filled++;
                while (j>0 && pd[j-1] > d) { pd[j]=pd[j-1]; pc[j]=pc[j-1]; j--; }
                pd[j]=d; pc[j]=c;
            } else if (d < pd[P-1]) {
                int j = P-1;
                while (j>0 && pd[j-1] > d) { pd[j]=pd[j-1]; pc[j]=pc[j-1]; j--; }
                pd[j]=d; pc[j]=c;
            }
        }
        for (int i=0;i<filled;i++) {
            uint32_t c = pc[i];
            uint32_t lo = ds->clust_pt_off[c], hi = ds->clust_pt_off[c+1];
            scan_cluster(ds, q8, lo, hi-lo, &C);
        }
    }

    int64_t bd[KNN_K]; uint32_t bo[KNN_K], bi[KNN_K];
    for (int j=0;j<KNN_K;j++){ bd[j]=INT64_MAX; bo[j]=UINT32_MAX; bi[j]=0; }
    for (int c=0;c<C.filled;c++)
        top_insert(dist16v(qv, ds->vecs16+(size_t)C.ci[c]*VDIM8), ds->orig[C.ci[c]], C.ci[c], bd, bo, bi);

    int fr=0; for(int j=0;j<KNN_K;j++) if(bd[j]!=INT64_MAX && is_fraud(ds,bi[j])) fr++;
    return fr;
}

/* Exact full brute force over all refs (verify only). */
int knn_fraud_count_full(const Dataset *ds, const int16_t q16[VLANES]) {
    int64_t bd[KNN_K]; uint32_t bo[KNN_K], bi[KNN_K];
    for(int j=0;j<KNN_K;j++){bd[j]=INT64_MAX;bo[j]=UINT32_MAX;bi[j]=0;}
    static const int dmap8[VDIM8]=DMAP8_INIT;
    int16_t qv[VLANES];
    for(int k=0;k<VDIM8;k++)qv[k]=q16[dmap8[k]];
    for(int k=VDIM8;k<VLANES;k++)qv[k]=0;
    int qbits=(q16[9]==SCALE)|((q16[10]==SCALE)<<1)|((q16[11]==SCALE)<<2);
    const int16_t *b16=ds->vecs16;
    for(int bucket=0;bucket<NBUCKETS;bucket++){
        int diff=qbits^(bucket&7); int64_t cd=0;
        for(int bit=0;bit<3;bit++) if(diff&(1<<bit)) cd+=(int64_t)SCALE*SCALE;
        for(uint32_t i=ds->hdr->bucket_off[bucket];i<ds->hdr->bucket_off[bucket+1];i++){
            int64_t d=cd+dist16v(qv,b16+(size_t)i*VDIM8); uint32_t oi=ds->orig[i];
            if(d>bd[KNN_K-1]||(d==bd[KNN_K-1]&&oi>=bo[KNN_K-1]))continue;
            int j=KNN_K-1; while(j>0&&(d<bd[j-1]||(d==bd[j-1]&&oi<bo[j-1]))){bd[j]=bd[j-1];bo[j]=bo[j-1];bi[j]=bi[j-1];j--;}
            bd[j]=d;bo[j]=oi;bi[j]=i;
        }
    }
    int fr=0; for(int j=0;j<KNN_K;j++) if(bd[j]!=INT64_MAX&&is_fraud(ds,bi[j]))fr++;
    return fr;
}
