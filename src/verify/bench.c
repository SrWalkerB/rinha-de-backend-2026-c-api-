/* bench.c — pure compute microbenchmark for vec_build + knn (no HTTP/network).
 * Usage: bench <packed.bin> <test-data.json> [loops] */
#define _GNU_SOURCE
#include "../common/fraud.h"
#include "../common/reqparse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) { perror("open"); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(1); }
    b[n] = '\0'; fclose(f); *len = (size_t)n; return b;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec*1e-9; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <packed.bin> <test-data.json> [loops]\n", argv[0]); return 1; }
    int loops = (argc > 3) ? atoi(argv[3]) : 5;

    Dataset ds;
    if (ds_open(&ds, argv[1]) != 0) return 1;

    size_t flen; char *buf = read_all(argv[2], &flen);

    /* pre-vectorize all entries */
    int cap = 60000, n = 0;
    int16_t (*Q)[VLANES] = malloc((size_t)cap * sizeof(*Q));
    int *KEY = malloc((size_t)cap * sizeof(int));
    const char *cur = buf;
    for (;;) {
        const char *rq = strstr(cur, "\"request\""); if (!rq) break;
        Request req;
        if (req_parse(rq, &req) == 0) { KEY[n] = vec_build(&req, Q[n]); n++; }
        const char *nx = strstr(rq, "\"expected_approved\"");
        cur = nx ? nx + 1 : rq + 9;
        if (n >= cap) break;
    }
    const char *ql = getenv("QLIMIT");
    if (ql && atoi(ql) > 0 && atoi(ql) < n) n = atoi(ql);
    fprintf(stderr, "queries=%d\n", n);

    /* warm */
    long sink = 0;
    for (int i = 0; i < n; i++) sink += knn_fraud_count(&ds, Q[i], KEY[i]);

    double t0 = now();
    for (int r = 0; r < loops; r++)
        for (int i = 0; i < n; i++) sink += knn_fraud_count(&ds, Q[i], KEY[i]);
    double dt = now() - t0;

    long total = (long)n * loops;
    printf("knn: %ld queries in %.3fs  => %.1f us/query  %.0f q/s  (sink=%ld)\n",
           total, dt, dt / total * 1e6, total / dt, sink);

    /* per-bucket timing */
    double bt[NBUCKETS] = {0}; long bc[NBUCKETS] = {0};
    for (int r = 0; r < (loops < 3 ? loops : 3); r++)
        for (int i = 0; i < n; i++) {
            int k = KEY[i];
            double a = now();
            sink += knn_fraud_count(&ds, Q[i], k);
            bt[k] += now() - a; bc[k]++;
        }
    uint32_t *bo = (uint32_t *)ds.hdr->bucket_off;
    for (int b = 0; b < NBUCKETS; b++)
        if (bc[b])
            printf("  bucket %2d: size=%-8u queries=%-6ld %.1f us/query\n",
                   b, bo[b+1]-bo[b], bc[b], bt[b]/bc[b]*1e6);
    return 0;
}
