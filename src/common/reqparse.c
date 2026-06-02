/* reqparse.c — sequential forward scan of the known payload layout:
 *   id, transaction{amount,installments,requested_at},
 *   customer{avg_amount,tx_count_24h,known_merchants[]},
 *   merchant{id,mcc,avg_amount}, terminal{is_online,card_present,km_from_home},
 *   last_transaction: null | {timestamp,km_from_current}
 * Keys are unique enough in document order that a forward strstr never collides
 * (e.g. "merchant" does not match inside "known_merchants"). */
#define _GNU_SOURCE
#include "reqparse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* pointer just past the ':' following `key` (searched from p forward), or NULL */
static const char *after_key(const char *p, const char *key) {
    const char *k = strstr(p, key);
    if (!k) return NULL;
    k = strchr(k, ':');
    return k ? k + 1 : NULL;
}
static double rd_dbl(const char **pp, const char *key) {
    const char *v = after_key(*pp, key);
    if (!v) return 0.0;
    char *e; double d = strtod(v, &e);
    *pp = e; return d;
}
static int rd_int(const char **pp, const char *key) {
    const char *v = after_key(*pp, key);
    if (!v) return 0;
    char *e; long n = strtol(v, &e, 10);
    *pp = e; return (int)n;
}
static int rd_bool(const char **pp, const char *key) {
    const char *v = after_key(*pp, key);
    if (!v) return 0;
    while (*v == ' ') v++;
    *pp = v + 1; return *v == 't';
}
/* copy "..." string value into dst (bounded), advance *pp past closing quote */
static void rd_str(const char **pp, const char *key, char *dst, int cap) {
    const char *v = after_key(*pp, key);
    if (!v) { dst[0] = '\0'; return; }
    while (*v && *v != '"') v++;
    if (!*v) { dst[0] = '\0'; return; }
    v++;
    int i = 0;
    while (*v && *v != '"' && i < cap - 1) dst[i++] = *v++;
    dst[i] = '\0';
    *pp = (*v == '"') ? v + 1 : v;
}

int req_parse(const char *body, Request *r) {
    const char *p = body;
    memset(r, 0, sizeof(*r));

    const char *tx = strstr(p, "\"transaction\"");
    if (!tx) return -1;
    p = tx;
    r->amount       = rd_dbl(&p, "\"amount\"");
    r->installments = rd_int(&p, "\"installments\"");
    rd_str(&p, "\"requested_at\"", r->requested_at, sizeof(r->requested_at));

    const char *cust = strstr(p, "\"customer\"");
    if (!cust) return -1;
    p = cust;
    r->cust_avg     = rd_dbl(&p, "\"avg_amount\"");
    r->tx_count_24h = rd_int(&p, "\"tx_count_24h\"");

    /* known_merchants array bounds (membership tested once we have merchant.id) */
    const char *km = strstr(p, "\"known_merchants\"");
    const char *arr_lo = NULL, *arr_hi = NULL;
    if (km) {
        arr_lo = strchr(km, '[');
        arr_hi = arr_lo ? strchr(arr_lo, ']') : NULL;
        p = arr_hi ? arr_hi : km;
    }

    const char *merch = strstr(p, "\"merchant\"");
    if (!merch) return -1;
    p = merch;
    char merch_id[32];
    rd_str(&p, "\"id\"", merch_id, sizeof(merch_id));
    rd_str(&p, "\"mcc\"", r->mcc, sizeof(r->mcc));
    r->merch_avg = rd_dbl(&p, "\"avg_amount\"");

    /* unknown_merchant = merchant.id NOT found in known_merchants */
    r->unknown_merchant = 1;
    if (arr_lo && arr_hi && merch_id[0]) {
        char needle[34];
        int n = snprintf(needle, sizeof(needle), "\"%s\"", merch_id);
        const char *q = arr_lo;
        while (q + n <= arr_hi) {
            if (memcmp(q, needle, (size_t)n) == 0) { r->unknown_merchant = 0; break; }
            q++;
        }
    }

    const char *term = strstr(p, "\"terminal\"");
    if (!term) return -1;
    p = term;
    r->is_online    = rd_bool(&p, "\"is_online\"");
    r->card_present = rd_bool(&p, "\"card_present\"");
    r->km_home      = rd_dbl(&p, "\"km_from_home\"");

    const char *last = strstr(p, "\"last_transaction\"");
    if (last) {
        const char *v = strchr(last, ':');
        if (v) {
            v++;
            while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t') v++;
            if (*v == '{') {
                p = v;
                rd_str(&p, "\"timestamp\"", r->last_ts, sizeof(r->last_ts));
                r->last_km  = rd_dbl(&p, "\"km_from_current\"");
                r->has_last = 1;
            }
        }
    }
    return 0;
}
