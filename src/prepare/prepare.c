/* prepare.c — build-time: references.json.gz -> packed.bin.
 *
 * Streams the gzip, scans each {"vector":[14 nums],"label":"fraud|legit"}
 * record with strtod, quantizes to int16 = round(v*10000) (lossless on the
 * round4 grid) and int8 = round(v*127) (coarse filter), groups records into 16
 * buckets (is_online, card_present, unknown_merchant, has_last), and writes:
 *   vecs8[bucket order] | vecs16[11 variable dims] | orig[original index] | fraud bitset
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

size_t packed_vecs8_off(void);
size_t packed_vecs16_off(uint32_t nrefs);
size_t packed_orig_off(uint32_t nrefs);
size_t packed_fraud_off(uint32_t nrefs);
size_t packed_total(uint32_t nrefs);

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
            frd = realloc(frd, cap);
            bk  = realloc(bk,  cap);
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
        lp = strchr(lp, ':'); lp++;
        while (*lp != '"') lp++;
        lp++;
        frd[n] = (lp[0] == 'f');
        bk[n]  = (uint8_t)bucket_of(dst);
        p = lp;
        n++;
    }
    free(buf);
    fprintf(stderr, "parsed %zu records\n", n);

    uint32_t counts[NBUCKETS] = {0};
    for (size_t i = 0; i < n; i++) counts[bk[i]]++;
    uint32_t off[NBUCKETS + 1]; off[0] = 0;
    for (int b = 0; b < NBUCKETS; b++) off[b + 1] = off[b] + counts[b];

    /* stable bucket-group: ORD[pos] = original index */
    uint32_t *ord = malloc((size_t)n * sizeof(uint32_t));
    uint32_t cur[NBUCKETS];
    if (!ord) { fprintf(stderr, "oom\n"); return 1; }
    for (int b = 0; b < NBUCKETS; b++) cur[b] = off[b];
    for (size_t i = 0; i < n; i++) ord[cur[bk[i]]++] = (uint32_t)i;

    /* emit arrays in bucket-grouped order */
    static const int dmap8[VDIM8] = DMAP8_INIT;
    int8_t   *ov8 = calloc((size_t)n * VDIM8 + 64, 1);   /* +64: safe over-read */
    int16_t  *ov16 = malloc((size_t)n * VDIM8 * sizeof(int16_t));
    uint32_t *os  = malloc((size_t)n * sizeof(uint32_t));
    uint8_t  *of  = calloc((n + 7) / 8, 1);
    if (!ov8 || !ov16 || !os || !of) { fprintf(stderr, "oom\n"); return 1; }
    for (size_t pos = 0; pos < n; pos++) {
        uint32_t src = ord[pos];
        const int16_t *v = qv + (size_t)src * VLANES;
        int16_t *d16 = ov16 + pos * VDIM8;
        int8_t *d8 = ov8 + pos * VDIM8;
        for (int k = 0; k < VDIM8; k++) {
            d16[k] = v[dmap8[k]];
            d8[k] = q16_to_q8(v[dmap8[k]]);
        }
        os[pos] = src;
        if (frd[src]) of[pos >> 3] |= (uint8_t)(1u << (pos & 7));
    }
    free(qv); free(frd); free(bk); free(ord);

    PackedHeader h = {0};
    h.magic = PACKED_MAGIC; h.version = PACKED_VERSION;
    h.vdim = VDIM; h.vlanes = VLANES; h.nrefs = (uint32_t)n; h.nbuckets = NBUCKETS;
    memcpy(h.bucket_off, off, sizeof(off));

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror("fopen out"); return 1; }
    char pad[64] = {0};
    size_t pos = 0;
    fwrite(&h, sizeof(h), 1, f); pos += sizeof(h);
    fwrite(pad, packed_vecs8_off() - pos, 1, f); pos = packed_vecs8_off();
    fwrite(ov8, sizeof(int8_t), (size_t)n * VDIM8, f); pos += (size_t)n * VDIM8;
    /* gap (incl. +64 over-read margin + 64B align) up to vecs16 */
    while (pos < packed_vecs16_off((uint32_t)n)) { size_t c = packed_vecs16_off((uint32_t)n) - pos; if (c > 64) c = 64; fwrite(pad, 1, c, f); pos += c; }
    fwrite(ov16, sizeof(int16_t), (size_t)n * VDIM8, f);
    fwrite(os, sizeof(uint32_t), n, f);
    fwrite(of, 1, (n + 7) / 8, f);
    fclose(f);

    fprintf(stderr, "wrote %s  (%zu refs, %.1f MB)\n", argv[2], n, packed_total((uint32_t)n) / 1048576.0);
    for (int b = 0; b < NBUCKETS; b++) fprintf(stderr, "  bucket %2d: %u\n", b, counts[b]);
    free(ov8); free(ov16); free(os); free(of);
    return 0;
}
