/* fraud.h — shared types and the bit-exact detection core.
 *
 * Ground truth = rinha-de-backend-2026/data-generator/main.c:
 *   - normalize()    (L523-555)  14-dim vector
 *   - round4(v)      (L216)      round(v*10000)/10000
 *   - knn_classify() (L570-598)  squared-euclidean 5-NN, strict-< insert,
 *                                lower original index wins ties
 *   - decision       approved = (frauds/5) < 0.6   (strict)
 *
 * We represent every dimension as int16 q = lround(value*10000). All reference
 * values are on the k/10000 grid (round4), so this is lossless and integer
 * squared distance reproduces the double ordering exactly. Sentinel -1 -> -10000.
 */
#ifndef FRAUD_H
#define FRAUD_H

#include <stdint.h>
#include <stddef.h>

#define VDIM      14        /* real dimensions                                */
#define VLANES    16        /* int16 padded to 16 lanes (one AVX2 load/ref)    */
#define VDIM8     11        /* int8 filter stores only the 11 non-constant dims*/
/* dims 9,10,11 (is_online, card_present, unknown_merchant) are CONSTANT within
 * every bucket (they ARE the bucket key) → contribute 0 to in-bucket distance →
 * dropped from the int8 first pass. Kept (in this lane order): the DMAP8 set.   */
#define DMAP8_INIT { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 13 }
#define KNN_K     5
#define NBUCKETS  16        /* key = is_online|card_present<<1|unknown<<2|has_last<<3 */
#define SCALE     10000     /* int16 fixed-point scale (lossless on round4)    */
#define SCALE8    127       /* int8 fixed-point scale (coarse filter)          */
#define SENTINEL  (-10000)  /* dims 5,6 when last_transaction == null (int16)  */
#define SENTINEL8 (-127)    /* sentinel in int8 space                          */
#define KNN_CAND  64        /* int8 first-pass candidates re-ranked exactly    */

/* fraud_score >= 0.6  => denied. 3 of 5 neighbors fraud = 0.6 exactly.       */
#define FRAUD_DENY_COUNT 3  /* frauds needed to deny (score>=0.6)             */

/* ── parsed request (only the fields the vector needs) ────────────────────── */
typedef struct {
    double  amount;
    int     installments;
    char    requested_at[32];   /* ISO-8601 "YYYY-MM-DDThh:mm:ssZ"            */
    double  cust_avg;
    int     tx_count_24h;
    /* unknown_merchant: 1 if merchant.id NOT in known_merchants              */
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

/* ── packed.bin on-disk layout (little-endian, AoS) ───────────────────────── */
#define PACKED_MAGIC   0x52484E41u  /* "ANHR" little-endian of 'RNHA'         */
#define PACKED_VERSION 6u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t vdim;                  /* 14                                      */
    uint32_t vlanes;                /* 16                                      */
    uint32_t nrefs;                 /* total reference vectors                 */
    uint32_t nbuckets;              /* 16                                      */
    uint32_t bucket_off[NBUCKETS+1];/* prefix sums (bucket-grouped, orig order)*/
    /* followed by (vecs start 64B-aligned):                                   */
    /*   int8_t   vecs8[nrefs * VDIM8]   (AoS, 11-dim coarse filter, grouped)   */
    /*   int16_t  vecs16[nrefs * VDIM8]  (AoS, exact re-rank, 11 variable dims) */
    /*   uint32_t orig[nrefs]            (original reference index, for ties)   */
    /*   uint8_t  fraud[(nrefs+7)/8]     (1=fraud, same order)                  */
} PackedHeader;

/* ── mmap'd dataset handle ────────────────────────────────────────────────── */
typedef struct {
    const PackedHeader *hdr;
    const int8_t       *vecs8;   /* nrefs * VLANES, coarse first pass          */
    const int16_t      *vecs16;  /* nrefs * VDIM8, exact re-rank              */
    const uint32_t     *orig;    /* nrefs, original index (tie-break)          */
    const uint8_t      *fraud;   /* bitset                                    */
    uint32_t            nrefs;
    void               *map_base;
    size_t              map_len;
} Dataset;

/* mcc_risk table (baked constants from resources/mcc_risk.json) -------------- */
double mcc_risk_lookup(const char *code);   /* default 0.5                     */

/* ── vectorization (bit-exact port of normalize()) ────────────────────────── */
/* Fills q[VLANES] (lanes 14,15 = 0) and returns bucket key 0..15.            */
int  vec_build(const Request *req, int16_t q[VLANES]);
int  vec_bucket_key(const Request *req);

/* ── dataset load / knn ───────────────────────────────────────────────────── */
int  ds_open(Dataset *ds, const char *path);   /* 0 ok, -1 fail               */
void ds_close(Dataset *ds);

/* Returns frauds among the 5 nearest within the query's bucket.              */
int  knn_fraud_count(const Dataset *ds, const int16_t q[VLANES], int bucket_key);

/* Full brute force over ALL refs (verify only). bucket_key ignored.         */
int  knn_fraud_count_full(const Dataset *ds, const int16_t q[VLANES]);

#endif /* FRAUD_H */
