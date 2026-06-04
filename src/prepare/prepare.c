/* prepare.c — build-time: references.json.gz -> packed.bin (v12, per-bucket IVF).
 *
 * Parse, quantize to int16 (lossless on the round4 grid), group into 16 binary
 * buckets, then k-means each bucket into clusters of ~TARGET_CLUSTER points.
 * Points are emitted grouped by (bucket, cluster):
 *   centroids   int16[nclust*VLANES] DMAP8-order, padded (query-time probe)
 *   clust_pt_off uint32[nclust+1]    point-index range of each cluster
 *   vecs16      point-major int16 (exact re-rank)
 *   vecs8       per-CLUSTER DIM-PAIR-interleaved int8 (SoA) for the vpmaddwd scan
 *   orig        original index (tie-break)   |   fraud bitset
 *
 * Usage: prepare <references.json.gz> <out packed.bin>
 */
#define _GNU_SOURCE
#include "../common/fraud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

static inline int8_t q16_to_q8(int16_t v) {
    long r = lround((double)v * (double)SCALE8 / (double)SCALE);
    if (r >  127) r =  127;
    if (r < -127) r = -127;
    return (int8_t)r;
}

static char *gunzip_all(const char *path, size_t *out_len) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    size_t cap = 64u << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + (16u << 20) + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
        if (!buf) { fprintf(stderr, "oom\n"); exit(1); }
        int n = gzread(gz, buf + len, 16u << 20);
        if (n < 0) { fprintf(stderr, "gzread error\n"); exit(1); }
        if (n == 0) break;
        len += (size_t)n;
    }
    gzclose(gz);
    buf[len] = '\0'; *out_len = len;
    return buf;
}

static int bucket_of(const int16_t *q) {
    return (q[9] == SCALE) | ((q[10] == SCALE) << 1) | ((q[11] == SCALE) << 2) | ((q[5] != SENTINEL) << 3);
}
static size_t align_up(size_t x, size_t a){ return (x+a-1)&~(a-1); }

/* squared distance over 16 int16 lanes (extra lanes are zero-padded). */
static inline int64_t dist16(const int16_t *a, const int16_t *b) {
#if defined(__AVX2__)
    __m256i va = _mm256_loadu_si256((const __m256i *)a);
    __m256i vb = _mm256_loadu_si256((const __m256i *)b);
    __m256i d  = _mm256_sub_epi16(va, vb);
    __m256i m  = _mm256_madd_epi16(d, d);
    __m128i lo = _mm256_castsi256_si128(m), hi = _mm256_extracti128_si256(m, 1);
    __m128i s  = _mm_add_epi32(lo, hi);
    __m128i s0 = _mm_cvtepi32_epi64(s);
    __m128i s1 = _mm_cvtepi32_epi64(_mm_srli_si128(s, 8));
    __m128i sum= _mm_add_epi64(s0, s1);
    return (int64_t)_mm_extract_epi64(sum,0)+(int64_t)_mm_extract_epi64(sum,1);
#else
    int64_t s=0; for(int k=0;k<VDIM8;k++){int32_t e=(int32_t)a[k]-b[k];s+=(int64_t)e*e;} return s;
#endif
}

/* deterministic LCG */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static inline uint32_t rnd(void){ g_rng = g_rng*6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(g_rng>>33); }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <refs.json.gz> <out.bin>\n", argv[0]); return 1; }

    size_t blen;
    char *buf = gunzip_all(argv[1], &blen);
    fprintf(stderr, "decompressed %.1f MB\n", blen / 1048576.0);

    size_t cap = 1u << 22, n = 0;
    int16_t *qv  = malloc(cap * VLANES * sizeof(int16_t));
    uint8_t *frd = malloc(cap);
    uint8_t *bk  = malloc(cap);
    if (!qv || !frd || !bk) { fprintf(stderr, "oom\n"); return 1; }

    char *p = buf;
    while ((p = strstr(p, "\"vector\"")) != NULL) {
        p = strchr(p, '['); if (!p) break; p++;
        if (n >= cap) {
            cap *= 2;
            qv  = realloc(qv,  cap * VLANES * sizeof(int16_t));
            frd = realloc(frd, cap); bk = realloc(bk, cap);
            if (!qv || !frd || !bk) { fprintf(stderr, "oom\n"); return 1; }
        }
        int16_t *dst = qv + n * VLANES;
        for (int k = 0; k < VDIM; k++) {
            char *end; double d = strtod(p, &end); p = end;
            while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ']') p++;
            dst[k] = (int16_t)lround(d * 10000.0);
        }
        for (int k = VDIM; k < VLANES; k++) dst[k] = 0;
        char *lp = strstr(p, "\"label\""); if (!lp) break;
        lp = strchr(lp, ':'); lp++; while (*lp != '"') lp++; lp++;
        frd[n] = (lp[0] == 'f'); bk[n] = (uint8_t)bucket_of(dst); p = lp; n++;
    }
    free(buf);
    fprintf(stderr, "parsed %zu records\n", n);

    static const int dmap8[VDIM8] = DMAP8_INIT;
    uint32_t counts[NBUCKETS] = {0};
    for (size_t i = 0; i < n; i++) counts[bk[i]]++;
    uint32_t off[NBUCKETS + 1]; off[0] = 0;
    for (int b = 0; b < NBUCKETS; b++) off[b + 1] = off[b] + counts[b];

    /* point indices grouped by bucket */
    uint32_t *ord = malloc((size_t)n * sizeof(uint32_t));
    if (!ord) { fprintf(stderr,"oom\n"); return 1; }
    { uint32_t cur[NBUCKETS]; for (int b=0;b<NBUCKETS;b++) cur[b]=off[b];
      for (size_t i=0;i<n;i++) ord[cur[bk[i]]++]=(uint32_t)i; }
    free(bk);

    /* ── per-bucket k-means → cluster label for each point, plus centroids ── */
    uint32_t clust_bucket_off[NBUCKETS+1]; clust_bucket_off[0]=0;
    uint32_t *clabel = malloc((size_t)n*sizeof(uint32_t));   /* cluster idx within bucket */
    int16_t  *all_cent = NULL; uint32_t cent_cap = 0, ncl_total = 0;
    if (!clabel) { fprintf(stderr,"oom\n"); return 1; }

    int tcl = TARGET_CLUSTER;
    { const char *e = getenv("TARGET_CLUSTER"); if (e && atoi(e) > 0) tcl = atoi(e); }
    fprintf(stderr, "TARGET_CLUSTER=%d\n", tcl);

    for (int b = 0; b < NBUCKETS; b++) {
        uint32_t base = off[b], cnt = counts[b];
        if (cnt == 0) { clust_bucket_off[b+1] = clust_bucket_off[b]; continue; }
        uint32_t C = (cnt + (uint32_t)tcl/2) / (uint32_t)tcl;
        if (C < 1) C = 1;
        if (C > cnt) C = cnt;

        /* padded int16 points for this bucket (DMAP8 order, lanes 11..15 = 0) */
        int16_t *pts = calloc((size_t)cnt*VLANES, sizeof(int16_t));
        int16_t *cent= calloc((size_t)C*VLANES, sizeof(int16_t));
        uint32_t *asg = malloc((size_t)cnt*sizeof(uint32_t));
        int64_t *sum  = malloc((size_t)C*VDIM8*sizeof(int64_t));
        uint32_t *ccnt= malloc((size_t)C*sizeof(uint32_t));
        if (!pts||!cent||!asg||!sum||!ccnt){ fprintf(stderr,"oom kmeans\n"); return 1; }
        for (uint32_t i=0;i<cnt;i++){
            const int16_t *v = qv + (size_t)ord[base+i]*VLANES;
            int16_t *d = pts + (size_t)i*VLANES;
            for (int k=0;k<VDIM8;k++) d[k]=v[dmap8[k]];
        }
        /* init: k-means++ (D^2 weighting) — better-spread seeds than strided,
         * which lifts IVF recall so E=0 is reached at a lower nprobe.
         * Toggle off with KMPP=0 (falls back to evenly strided). */
        int use_kmpp = 1; { const char *e=getenv("KMPP"); if(e&&atoi(e)==0) use_kmpp=0; }
        if (use_kmpp && C>1) {
            int64_t *mind = malloc((size_t)cnt*sizeof(int64_t));
            uint32_t s0 = rnd()%cnt;
            memcpy(cent, pts+(size_t)s0*VLANES, VLANES*sizeof(int16_t));
            int64_t tot=0;
            for (uint32_t i=0;i<cnt;i++){ mind[i]=dist16(pts+(size_t)i*VLANES, cent); tot+=mind[i]; }
            for (uint32_t j=1;j<C;j++){
                /* pick next seed with prob proportional to mind[] (D^2) */
                uint32_t pick;
                if (tot<=0) { pick = rnd()%cnt; }
                else {
                    int64_t target = (int64_t)(( (unsigned long long)rnd()<<31 | rnd() ) % (unsigned long long)tot);
                    int64_t acc=0; pick=cnt-1;
                    for (uint32_t i=0;i<cnt;i++){ acc+=mind[i]; if(acc>target){pick=i;break;} }
                }
                int16_t *cj = cent+(size_t)j*VLANES;
                memcpy(cj, pts+(size_t)pick*VLANES, VLANES*sizeof(int16_t));
                tot=0;
                for (uint32_t i=0;i<cnt;i++){ int64_t d=dist16(pts+(size_t)i*VLANES, cj); if(d<mind[i])mind[i]=d; tot+=mind[i]; }
            }
            free(mind);
        } else {
            for (uint32_t j=0;j<C;j++){
                uint32_t src = (uint32_t)(((uint64_t)j*cnt)/C);
                memcpy(cent+(size_t)j*VLANES, pts+(size_t)src*VLANES, VLANES*sizeof(int16_t));
            }
        }
        for (int it=0; it<KMEANS_ITERS; it++){
            memset(sum,0,(size_t)C*VDIM8*sizeof(int64_t));
            memset(ccnt,0,(size_t)C*sizeof(uint32_t));
            for (uint32_t i=0;i<cnt;i++){
                const int16_t *v = pts+(size_t)i*VLANES;
                int64_t best=INT64_MAX; uint32_t bj=0;
                for (uint32_t j=0;j<C;j++){
                    int64_t d=dist16(v, cent+(size_t)j*VLANES);
                    if (d<best){best=d;bj=j;}
                }
                asg[i]=bj; ccnt[bj]++;
                int64_t *s=sum+(size_t)bj*VDIM8;
                for (int k=0;k<VDIM8;k++) s[k]+=v[k];
            }
            for (uint32_t j=0;j<C;j++){
                int16_t *cj = cent+(size_t)j*VLANES;
                if (ccnt[j]==0){ /* reseed empty cluster to a random point */
                    uint32_t src = rnd()%cnt;
                    memcpy(cj, pts+(size_t)src*VLANES, VLANES*sizeof(int16_t));
                    continue;
                }
                int64_t *s=sum+(size_t)j*VDIM8;
                for (int k=0;k<VDIM8;k++) cj[k]=(int16_t)((s[k]>=0? s[k]+ccnt[j]/2 : s[k]-(int64_t)ccnt[j]/2)/ (int64_t)ccnt[j]);
            }
        }
        /* final assignment */
        for (uint32_t i=0;i<cnt;i++){
            const int16_t *v = pts+(size_t)i*VLANES;
            int64_t best=INT64_MAX; uint32_t bj=0;
            for (uint32_t j=0;j<C;j++){ int64_t d=dist16(v, cent+(size_t)j*VLANES); if(d<best){best=d;bj=j;} }
            clabel[base+i]=bj;
        }
        /* append centroids */
        if (ncl_total + C > cent_cap){ cent_cap = (ncl_total+C)*2 + 16; all_cent = realloc(all_cent, (size_t)cent_cap*VLANES*sizeof(int16_t)); if(!all_cent){fprintf(stderr,"oom cent\n");return 1;} }
        memcpy(all_cent + (size_t)ncl_total*VLANES, cent, (size_t)C*VLANES*sizeof(int16_t));
        ncl_total += C;
        clust_bucket_off[b+1] = ncl_total;

        free(pts);free(cent);free(asg);free(sum);free(ccnt);
        fprintf(stderr, "bucket %2d: %u pts -> %u clusters\n", b, cnt, C);
    }
    fprintf(stderr, "total clusters: %u\n", ncl_total);

    /* ── reorder points by (bucket, cluster); build clust_pt_off ── */
    /* cluster sizes */
    uint32_t *clsize = calloc((size_t)ncl_total, sizeof(uint32_t));
    if (!clsize){fprintf(stderr,"oom\n");return 1;}
    for (int b=0;b<NBUCKETS;b++){
        uint32_t base=off[b], cnt=counts[b], cb0=clust_bucket_off[b];
        for (uint32_t i=0;i<cnt;i++) clsize[cb0 + clabel[base+i]]++;
    }
    uint32_t *clust_pt_off = malloc(((size_t)ncl_total+1)*sizeof(uint32_t));
    clust_pt_off[0]=0;
    for (uint32_t c=0;c<ncl_total;c++) clust_pt_off[c+1]=clust_pt_off[c]+clsize[c];

    /* final point order: position of each (bucket point) in the (bucket,cluster) layout */
    uint32_t *neworder = malloc((size_t)n*sizeof(uint32_t)); /* neworder[pos] = src index */
    uint32_t *cwrite = malloc((size_t)ncl_total*sizeof(uint32_t));
    for (uint32_t c=0;c<ncl_total;c++) cwrite[c]=clust_pt_off[c];
    for (int b=0;b<NBUCKETS;b++){
        uint32_t base=off[b], cnt=counts[b], cb0=clust_bucket_off[b];
        for (uint32_t i=0;i<cnt;i++){
            uint32_t c = cb0 + clabel[base+i];
            neworder[cwrite[c]++] = ord[base+i];
        }
    }
    free(ord); free(clabel); free(clsize); free(cwrite);

    /* ── emit point arrays in new order ── */
    int16_t  *ov16 = malloc((size_t)n*VDIM8*sizeof(int16_t));
    int8_t   *ov8  = calloc((size_t)n*VPAD + 64, 1);   /* per-cluster pair-SoA, +64 over-read margin */
    uint32_t *os   = malloc((size_t)n*sizeof(uint32_t));
    uint8_t  *of   = calloc((n+7)/8, 1);
    if (!ov16||!ov8||!os||!of){ fprintf(stderr, "oom emit\n"); return 1; }
    for (uint32_t c=0;c<ncl_total;c++){
        uint32_t cbase = clust_pt_off[c], ccnt = clust_pt_off[c+1]-clust_pt_off[c];
        int8_t *soa = ov8 + (size_t)cbase * VPAD;       /* this cluster's pair-SoA block */
        for (uint32_t i=0;i<ccnt;i++){
            uint32_t pos = cbase + i, src = neworder[pos];
            const int16_t *v = qv + (size_t)src*VLANES;
            int16_t *d16 = ov16 + (size_t)pos*VDIM8;
            for (int k=0;k<VDIM8;k++) d16[k]=v[dmap8[k]];
            for (int pp=0; pp<GPAIRS; pp++){
                int da=2*pp, db=2*pp+1;
                int8_t va=(da<VDIM8)?q16_to_q8(v[dmap8[da]]):0;
                int8_t vb=(db<VDIM8)?q16_to_q8(v[dmap8[db]]):0;
                soa[(size_t)pp*2*ccnt + (size_t)i*2 + 0]=va;
                soa[(size_t)pp*2*ccnt + (size_t)i*2 + 1]=vb;
            }
            os[pos]=src;
            if (frd[src]) of[pos>>3] |= (uint8_t)(1u<<(pos&7));
        }
    }
    free(qv); free(frd); free(neworder);

    /* ── per-bucket int8 pair-SoA centroids (batched probe scan, mirrors vecs8) ──
     * all_cent is int16 in DMAP8 order (lanes >=VDIM8 are 0). Quantize to int8 and
     * lay each bucket's centroids out dim-pair-interleaved so the query can scan 8
     * centroids at a time with vpmaddwd — no per-centroid horizontal reduction. */
    uint32_t cent8_bucket_off[NBUCKETS+1]; cent8_bucket_off[0]=0;
    for (int b=0;b<NBUCKETS;b++){
        uint32_t ncl_b = clust_bucket_off[b+1]-clust_bucket_off[b];
        cent8_bucket_off[b+1] = cent8_bucket_off[b] + ncl_b*(uint32_t)VPAD;
    }
    size_t cent8_len = cent8_bucket_off[NBUCKETS];
    int8_t *cent8 = calloc(cent8_len + 64, 1);
    if(!cent8){fprintf(stderr,"oom cent8\n");return 1;}
    for (int b=0;b<NBUCKETS;b++){
        uint32_t cb0 = clust_bucket_off[b];
        uint32_t ncl_b = clust_bucket_off[b+1]-cb0;
        int8_t *blk = cent8 + cent8_bucket_off[b];
        for (uint32_t i=0;i<ncl_b;i++){
            const int16_t *cv = all_cent + (size_t)(cb0+i)*VLANES; /* DMAP8 order */
            for (int pp=0; pp<GPAIRS; pp++){
                int da=2*pp, db=2*pp+1;
                int8_t va=(da<VDIM8)?q16_to_q8(cv[da]):0;
                int8_t vb=(db<VDIM8)?q16_to_q8(cv[db]):0;
                blk[(size_t)pp*2*ncl_b + (size_t)i*2 + 0]=va;
                blk[(size_t)pp*2*ncl_b + (size_t)i*2 + 1]=vb;
            }
        }
    }

    PackedHeader h = {0};
    h.magic=PACKED_MAGIC; h.version=PACKED_VERSION; h.vdim=VDIM; h.vlanes=VLANES;
    h.nrefs=(uint32_t)n; h.nbuckets=NBUCKETS; h.nclust=ncl_total;
    memcpy(h.bucket_off, off, sizeof(off));
    memcpy(h.clust_bucket_off, clust_bucket_off, sizeof(clust_bucket_off));
    memcpy(h.cent8_bucket_off, cent8_bucket_off, sizeof(cent8_bucket_off));

    size_t pos = align_up(sizeof(PackedHeader), 64);
    h.clust_pt_off_off = pos; pos += ((size_t)ncl_total+1)*sizeof(uint32_t);
    pos = align_up(pos, 64);
    h.centroids_off = pos; pos += (size_t)ncl_total*VLANES*sizeof(int16_t);
    pos = align_up(pos, 64);
    h.cent8_off = pos; pos += cent8_len + 64;
    pos = align_up(pos, 64);
    h.vecs16_off = pos; pos += (size_t)n*VDIM8*sizeof(int16_t);
    h.vecs8_off  = pos; pos += (size_t)n*VPAD + 64;
    h.orig_off   = pos; pos += (size_t)n*sizeof(uint32_t);
    h.fraud_off  = pos; pos += (n+7)/8;
    h.total_len  = pos;

    FILE *f = fopen(argv[2], "wb"); if(!f){perror("fopen out");return 1;}
    char pad[64]={0}; size_t at=0;
    #define PAD_TO(t) do{ while(at<(t)){ size_t c=(t)-at; if(c>64)c=64; fwrite(pad,1,c,f); at+=c; } }while(0)
    fwrite(&h,sizeof(h),1,f); at+=sizeof(h);
    PAD_TO(h.clust_pt_off_off); fwrite(clust_pt_off,sizeof(uint32_t),(size_t)ncl_total+1,f); at+=((size_t)ncl_total+1)*sizeof(uint32_t);
    PAD_TO(h.centroids_off);    fwrite(all_cent,sizeof(int16_t),(size_t)ncl_total*VLANES,f); at+=(size_t)ncl_total*VLANES*sizeof(int16_t);
    PAD_TO(h.cent8_off);  fwrite(cent8,1,cent8_len+64,f); at+=cent8_len+64;
    PAD_TO(h.vecs16_off); fwrite(ov16,sizeof(int16_t),(size_t)n*VDIM8,f); at+=(size_t)n*VDIM8*sizeof(int16_t);
    PAD_TO(h.vecs8_off);  fwrite(ov8,1,(size_t)n*VPAD+64,f); at+=(size_t)n*VPAD+64;
    PAD_TO(h.orig_off);   fwrite(os,sizeof(uint32_t),n,f); at+=(size_t)n*sizeof(uint32_t);
    PAD_TO(h.fraud_off);  fwrite(of,1,(n+7)/8,f); at+=(n+7)/8;
    fclose(f);

    fprintf(stderr,"wrote %s  (%zu refs, %u clusters, %.1f MB)\n",argv[2],n,ncl_total,h.total_len/1048576.0);
    free(ov16);free(ov8);free(os);free(of);free(clust_pt_off);free(all_cent);free(cent8);
    return 0;
}
