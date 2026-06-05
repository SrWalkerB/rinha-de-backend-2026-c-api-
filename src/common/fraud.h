/* fraud.h — shared types and the bit-exact detection core.
 *
 * Ground truth = rinha-de-backend-2026/data-generator/main.c:
 *   - normalize()    (L523-555)  14-dim vector
 *   - round4(v)      (L216)      round(v*10000)/10000
 *   - knn_classify() (L570-598)  squared-euclidean 5-NN, strict-< insert,
 *                                lower original index wins ties
 *   - decision       approved = (frauds/5) < 0.6   (strict)
 *
 * int16 q = lround(value*10000) is lossless on the round4 grid, so integer
 * squared distance reproduces the double ordering exactly. Sentinel -1 -> -10000.
 *
 * v12: per-bucket IVF (k-means inverted file). Each of the 16 binary buckets is
 * partitioned offline into clusters; points are stored grouped by (bucket,
 * cluster). At query time we score the bucket's cluster centroids, probe the
 * NPROBE nearest, and run the int8 vpmaddwd coarse scan + int16 exact re-rank
 * only over those clusters' points. This turns the full per-bucket brute force
 * (up to 1M points / >1ms) into a few-thousand-candidate scan (~tens of us),
 * which is what lets the server keep up with the arrival rate and collapse p99.
 * NPROBE is tuned so the probed clusters contain the true 5-NN for every test
 * query (verify.c proves 0 detection disagreement vs the exact labels). */
#ifndef FRAUD_H
#define FRAUD_H

#include <stdint.h>
#include <stddef.h>

#define VDIM      14
#define VLANES    16
#define VDIM8     11        /* stored variable dims (the DMAP8 set)            */
#define VPAD      12        /* padded to even for dim-pairing (dim 11 = 0)     */
#define GPAIRS    6         /* VPAD/2 dim-pairs                                 */
#define DMAP8_INIT { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 13 }
#define KNN_K     5
#define NBUCKETS  16
#define SCALE     10000
#define SCALE8    127
#define SENTINEL  (-10000)
#define KNN_CAND  64        /* int8 first-pass candidates re-ranked exactly    */

#define FRAUD_DENY_COUNT 3

/* IVF build/query parameters. */
#define TARGET_CLUSTER 75   /* aim for ~this many points per k-means cluster.
                               Finer than v12's 600: ~6x fewer points scanned
                               per query at E=0 (138k->22k pts) — the dominant
                               compute term, so ~2x lower latency. Centroid count
                               rises (~40k total) but stays the cheaper term at
                               nprobe=256. See knn.c.                            */
#define KMEANS_ITERS   12   /* Lloyd iterations at build time                  */
#define NPROBE_DEFAULT 24   /* first-pass clusters probed per query.
                               Non-extreme 1..4-fraud results are confirmed with
                               NPROBE_CONFIRM; public full-test parity stays E=0
                               while cutting the hot KNN path versus fixed 192.  */
#define NPROBE_CONFIRM 192
#define NPROBE_MAX     512

typedef struct {
    double  amount;
    int     installments;
    char    requested_at[32];
    double  cust_avg;
    int     tx_count_24h;
    int     unknown_merchant;
    char    mcc[8];
    double  merch_avg;
    int     is_online;
    int     card_present;
    double  km_home;
    int     has_last;
    char    last_ts[32];
    double  last_km;
} Request;

#define PACKED_MAGIC   0x52484E41u
#define PACKED_VERSION 13u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t vdim;
    uint32_t vlanes;
    uint32_t nrefs;
    uint32_t nbuckets;
    uint32_t nclust;                    /* total clusters across all buckets       */
    uint32_t clust_bucket_off[NBUCKETS+1]; /* cluster-index range per bucket        */
    uint32_t bucket_off[NBUCKETS+1];    /* point-index range per bucket            */
    uint32_t cent8_bucket_off[NBUCKETS+1]; /* int8 element offset of each bucket's
                                              centroid pair-SoA block in cent8      */
    uint64_t clust_pt_off_off;          /* uint32 clust_pt_off[nclust+1] (pt index)*/
    uint64_t centroids_off;             /* int16 centroids[nclust*VLANES] DMAP8 pad*/
    uint64_t cent8_off;    /* int8, per BUCKET dim-PAIR-interleaved SoA centroids
                              (VPAD per centroid) — the batched probe scan          */
    uint64_t vecs16_off;   /* int16 vecs16[nrefs*VDIM8], point-major (re-rank)        */
    uint64_t vecs8_off;    /* int8, per CLUSTER dim-PAIR-interleaved SoA (VPAD/point) */
    uint64_t orig_off;     /* uint32 orig[nrefs]                                      */
    uint64_t fraud_off;    /* uint8 fraud bitset                                      */
    uint64_t total_len;
} PackedHeader;

typedef struct {
    const PackedHeader *hdr;
    const uint32_t     *clust_pt_off; /* clust_pt_off[nclust+1]                  */
    const int16_t      *centroids;    /* nclust * VLANES, DMAP8-order padded     */
    const int8_t       *cent8;        /* per-bucket pair-SoA int8 centroids:
                                         block base = cent8_bucket_off[bucket];
                                         pair pp at +pp*2*ncl; centroid i at +2*i */
    const int16_t      *vecs16;  /* point-major, exact re-rank                  */
    const int8_t       *vecs8;   /* per-cluster pair-SoA: cluster base =
                                    clust_pt_off[c]*VPAD; pair pp at +pp*2*cnt;
                                    point i at +2*i                              */
    const uint32_t     *orig;
    const uint8_t      *fraud;
    uint32_t            nrefs;
    void               *map_base;
    size_t              map_len;
} Dataset;

double mcc_risk_lookup(const char *code);

int  vec_build(const Request *req, int16_t q[VLANES]);
int  vec_bucket_key(const Request *req);

int  ds_open(Dataset *ds, const char *path);
void ds_close(Dataset *ds);

/* nprobe<=0 => probe every cluster in the bucket (exact within bucket). */
int  knn_fraud_count(const Dataset *ds, const int16_t q[VLANES], int bucket_key, int nprobe);
int  knn_fraud_count_adaptive(const Dataset *ds, const int16_t q[VLANES], int bucket_key, int nprobe);
int  knn_fraud_count_full(const Dataset *ds, const int16_t q[VLANES]);

#endif /* FRAUD_H */
