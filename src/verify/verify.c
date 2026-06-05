/* verify.c — offline parity gate.
 * Runs our pipeline (parse -> vec_build -> bucketed knn) over every entry in
 * test/test-data.json and compares `approved` against expected_approved (the
 * authoritative main.c labels). Also spot-checks bucketed vs full brute force.
 *
 * Usage: verify <packed.bin> <test-data.json> [full_check_count]
 * Exit 0 iff zero approved-disagreements vs expected.
 */
#define _GNU_SOURCE
#include "../common/fraud.h"
#include "../common/reqparse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open test-data"); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(1); }
    b[n] = '\0'; fclose(f); *len = (size_t)n; return b;
}

static int read_bool_after(const char *p, const char *key) {
    const char *k = strstr(p, key);
    if (!k) return -1;
    k = strchr(k, ':'); if (!k) return -1;
    k++; while (*k == ' ') k++;
    return *k == 't';
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <packed.bin> <test-data.json> [full_n]\n", argv[0]); return 2; }
    long full_n = (argc > 3) ? atol(argv[3]) : 2000;

    int nprobe = NPROBE_DEFAULT;
    { const char *np = getenv("NPROBE"); if (np && *np) nprobe = atoi(np); }

    Dataset ds;
    if (ds_open(&ds, argv[1]) != 0) return 2;
    fprintf(stderr, "loaded %u refs (nprobe=%d)\n", ds.nrefs, nprobe);

    size_t flen; char *buf = read_all(argv[2], &flen);

    long total = 0, mism_expected = 0, mism_full = 0, parse_fail = 0;
    const char *cur = buf;
    for (;;) {
        const char *rq = strstr(cur, "\"request\"");
        if (!rq) break;

        Request req;
        if (req_parse(rq, &req) != 0) { parse_fail++; cur = rq + 9; continue; }

        int16_t q[VLANES];
        int key = vec_build(&req, q);
        int fb  = knn_fraud_count_adaptive(&ds, q, key, nprobe);
        int approved = (fb < FRAUD_DENY_COUNT);

        int exp = read_bool_after(rq, "\"expected_approved\"");
        if (exp >= 0) {
            total++;
            if (approved != exp) {
                mism_expected++;
                if (mism_expected <= 20)
                    fprintf(stderr, "MISMATCH#%ld: ours=%d exp=%d frauds=%d bucket=%d\n",
                            total, approved, exp, fb, key);
            }
        }
        if (total <= full_n) {
            int ff = knn_fraud_count_full(&ds, q);
            if (ff != fb) {
                mism_full++;
                if (mism_full <= 20)
                    fprintf(stderr, "BUCKET!=FULL #%ld: bucket=%d full=%d key=%d\n", total, fb, ff, key);
            }
        }

        const char *next = strstr(rq, "\"expected_approved\"");
        cur = next ? next + 1 : rq + 9;
    }

    printf("entries=%ld  parse_fail=%ld\n", total, parse_fail);
    printf("approved mismatches vs expected: %ld (%.4f%%)\n",
           mism_expected, total ? 100.0 * mism_expected / total : 0.0);
    printf("bucketed != full (first %ld): %ld\n", full_n, mism_full);

    ds_close(&ds);
    free(buf);
    return (mism_expected == 0 && mism_full == 0) ? 0 : 1;
}
