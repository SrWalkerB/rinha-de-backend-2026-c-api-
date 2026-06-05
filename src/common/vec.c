/* vec.c — bit-exact port of normalize() + round4 from data-generator/main.c. */
#define _GNU_SOURCE
#include "fraud.h"
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

/* resources/mcc_risk.json, baked. Default 0.5 (main.c mcc_lookup). */
static const struct { const char *code; double risk; } MCC[] = {
    {"5411",0.15},{"5812",0.30},{"5912",0.20},{"5944",0.45},{"7801",0.80},
    {"7802",0.75},{"7995",0.85},{"4511",0.35},{"5311",0.25},{"5999",0.50},
};
double mcc_risk_lookup(const char *code) {
    for (size_t i = 0; i < sizeof(MCC)/sizeof(MCC[0]); i++)
        if (strcmp(MCC[i].code, code) == 0) return MCC[i].risk;
    return 0.5;
}

static inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
static inline double round4(double v)  { return round(v * 10000.0) / 10000.0; }

/* fixed-offset ISO parsers: "YYYY-MM-DDThh:mm:ssZ" */
static inline int p2(const char *s) { return (s[0]-'0')*10 + (s[1]-'0'); }
static inline int p4(const char *s) { return (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0'); }

/* days since 1970-01-01 (Howard Hinnant), matches timegm for valid dates. */
static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}
static long ts_epoch_str(const char *ts) {       /* expects fixed format */
    return days_from_civil(p4(ts), p2(ts+5), p2(ts+8)) * 86400L
         + p2(ts+11) * 3600L + p2(ts+14) * 60L + p2(ts+17);
}

/* Sakamoto, Monday=0..Sunday=6 (main.c day_of_week). */
static int day_of_week(int y, int m, int d) {
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    int dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7; /* 0=Sun */
    return (dow + 6) % 7;                                  /* 0=Mon */
}

int vec_bucket_key(const Request *req) {
    return (req->is_online      ? 1 : 0)
         | (req->card_present   ? 2 : 0)
         | (req->unknown_merchant ? 4 : 0)
         | (req->has_last       ? 8 : 0);
}

int req_confirm_extreme(const Request *req, int bucket_key) {
    if (!(bucket_key == 13 || bucket_key == 14)) return 0;
    if (req->installments < 6 || req->installments > 10) return 0;
    if (req->amount < 2000.0 || req->amount > 5000.0) return 0;
    if (req->cust_avg < 110.0 || req->cust_avg > 300.0) return 0;
    if (req->tx_count_24h < 8 || req->tx_count_24h > 15) return 0;
    if (req->merch_avg > 90.0) return 0;
    if (req->km_home < 180.0 || req->km_home > 500.0) return 0;
    if (!req->has_last || req->last_km < 180.0 || req->last_km > 600.0) return 0;
    return strcmp(req->mcc, "7801") == 0 || strcmp(req->mcc, "7802") == 0 || strcmp(req->mcc, "7995") == 0;
}

int vec_build(const Request *req, int16_t q[VLANES]) {
    double v[VDIM];
    const char *ts = req->requested_at;
    int y, mo, d, h;
    if (ts[4] == '-' && ts[7] == '-' && ts[10] == 'T') {
        y = p4(ts); mo = p2(ts+5); d = p2(ts+8); h = p2(ts+11);
    } else { y = 2026; mo = 1; d = 1; h = 0; }
    int dow = day_of_week(y, mo, d);

    v[0]  = clamp01(req->amount / 10000.0);
    v[1]  = clamp01((double)req->installments / 12.0);
    v[2]  = clamp01((req->amount / (req->cust_avg != 0 ? req->cust_avg : 1e-9)) / 10.0);
    v[3]  = (double)h / 23.0;
    v[4]  = (double)dow / 6.0;
    if (req->has_last) {
        double mins = (double)(ts_epoch_str(req->requested_at) - ts_epoch_str(req->last_ts)) / 60.0;
        v[5] = clamp01(mins / 1440.0);
        v[6] = clamp01(req->last_km / 1000.0);
    } else {
        v[5] = -1.0;
        v[6] = -1.0;
    }
    v[7]  = clamp01(req->km_home / 1000.0);
    v[8]  = clamp01((double)req->tx_count_24h / 20.0);
    v[9]  = req->is_online ? 1.0 : 0.0;
    v[10] = req->card_present ? 1.0 : 0.0;
    v[11] = req->unknown_merchant ? 1.0 : 0.0;
    v[12] = mcc_risk_lookup(req->mcc);
    v[13] = clamp01(req->merch_avg / 10000.0);

    for (int i = 0; i < VDIM; i++)
        q[i] = (int16_t)lround(round4(v[i]) * 10000.0);
    for (int i = VDIM; i < VLANES; i++)
        q[i] = 0;

    return vec_bucket_key(req);
}
