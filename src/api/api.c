/* api.c — single-thread epoll HTTP/1.1 server for the fraud-score endpoint.
 *
 *   POST /fraud-score -> parse, vectorize, bucketed 5-NN, fixed JSON reply
 *   GET  /ready       -> 200 once packed.bin is mapped + warmed
 *
 * One thread per process (each instance is capped ~0.45 CPU; a thread can't use
 * more than one core anyway). Keep-alive, TCP_NODELAY, zero heap on hot path,
 * precomputed responses keyed by fraud-count (0..5).
 *
 * Usage: api <packed.bin> <port>
 */
#define _GNU_SOURCE
#include "../common/fraud.h"
#include "../common/reqparse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>

#define MAXEV    256
#define RBUF     2048
#define WBUF     1024
#define RESP_MAX 160     /* upper bound on one response's size */

static Dataset g_ds;
static int g_nprobe = NPROBE_DEFAULT;

/* Precomputed full HTTP responses, indexed by fraud_count 0..5. */
static char  g_resp[6][WBUF];
static int   g_resp_len[6];
static char  g_ready[128];
static int   g_ready_len;

static void build_responses(void) {
    static const char *body[6] = {
        "{\"approved\":true,\"fraud_score\":0}",
        "{\"approved\":true,\"fraud_score\":0.2}",
        "{\"approved\":true,\"fraud_score\":0.4}",
        "{\"approved\":false,\"fraud_score\":0.6}",
        "{\"approved\":false,\"fraud_score\":0.8}",
        "{\"approved\":false,\"fraud_score\":1}",
    };
    for (int i = 0; i < 6; i++)
        g_resp_len[i] = snprintf(g_resp[i], WBUF,
            "HTTP/1.1 200 OK\r\nContent-Type:application/json\r\n"
            "Content-Length:%zu\r\nConnection:keep-alive\r\n\r\n%s",
            strlen(body[i]), body[i]);
    g_ready_len = snprintf(g_ready, sizeof(g_ready),
        "HTTP/1.1 200 OK\r\nContent-Length:0\r\nConnection:keep-alive\r\n\r\n");
}

/* ── connection state ─────────────────────────────────────────────────────── */
typedef struct {
    int      fd;
    int      rlen;
    int      wlen, woff;
    uint32_t want;          /* currently-armed epoll events (avoid redundant MOD) */
    char     rbuf[RBUF];
    char     wbuf[WBUF];
} Conn;

#include <stdint.h>
static int g_ep;
static inline void want_events(Conn *c, uint32_t ev) {
    if (c->want == ev) return;                 /* no syscall when unchanged */
    c->want = ev;
    struct epoll_event e = { .events = ev, .data.fd = c->fd };
    epoll_ctl(g_ep, EPOLL_CTL_MOD, c->fd, &e);
}

static Conn *conns[1 << 16];   /* fd -> Conn */

static int content_length(const char *h, const char *hend) {
    /* case-insensitive scan for "content-length:" within headers */
    for (const char *p = h; p + 15 < hend; p++) {
        if (strncasecmp(p, "content-length:", 15) == 0) {
            p += 15;
            while (*p == ' ') p++;
            return atoi(p);
        }
    }
    return -1;
}

/* Build a response for the consumed request into c->wbuf. */
static void handle_one(Conn *c, const char *req, const char *body) {
    if (req[0] == 'G') {                         /* GET /ready */
        memcpy(c->wbuf + c->wlen, g_ready, g_ready_len);
        c->wlen += g_ready_len;
        return;
    }
    /* POST /fraud-score */
    int fn = 0;
    Request r;
    if (body && req_parse(body, &r) == 0) {
        int16_t q[VLANES];
        int key = vec_build(&r, q);
        fn = knn_fraud_count_policy(&g_ds, q, key, g_nprobe, req_confirm_extreme(&r, key));
    }
    memcpy(c->wbuf + c->wlen, g_resp[fn], g_resp_len[fn]);
    c->wlen += g_resp_len[fn];
}

/* Try to flush c->wbuf. Returns 0 ok (maybe partial), -1 on fatal error. */
static int flush_w(int ep, Conn *c) {
    (void)ep;
    while (c->woff < c->wlen) {
        ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
        if (n > 0) { c->woff += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            want_events(c, EPOLLIN | EPOLLOUT | EPOLLET);
            return 0;
        }
        return -1;
    }
    c->woff = c->wlen = 0;
    want_events(c, EPOLLIN | EPOLLET);   /* no-op when already armed (the hot path) */
    return 0;
}

static void close_conn(int ep, Conn *c) {
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conns[c->fd] = NULL;
    free(c);
}

/* Process all complete requests buffered in c->rbuf. Returns -1 to close. */
static int process(int ep, Conn *c) {
    int consumed = 0;
    for (;;) {
        char *base = c->rbuf + consumed;
        int   avail = c->rlen - consumed;
        if (avail <= 0) break;

        char *hend = memmem(base, avail, "\r\n\r\n", 4);
        if (!hend) break;                        /* headers incomplete         */
        int hlen = (int)(hend - base) + 4;

        int blen = 0;
        if (base[0] == 'P') {                    /* POST has a body            */
            int cl = content_length(base, hend);
            if (cl < 0) cl = 0;
            if (avail < hlen + cl) break;        /* body incomplete            */
            blen = cl;
            base[hlen + cl] = '\0';              /* NUL-terminate body (safe: WBUF/RBUF slack or last) */
        }
        const char *body = (base[0] == 'P') ? base + hlen : NULL;

        if (c->wlen + RESP_MAX > WBUF) {         /* no room; flush first        */
            if (flush_w(ep, c) < 0) return -1;
            if (c->wlen != 0) break;             /* still pending; stop pipelining */
        }
        handle_one(c, base, body);
        consumed += hlen + blen;
    }
    if (consumed > 0) {
        memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
        c->rlen -= consumed;
    }
    return flush_w(ep, c);
}

static void on_readable(int ep, Conn *c) {
    for (;;) {
        if (c->rlen >= RBUF) { close_conn(ep, c); return; }  /* oversized; drop */
        ssize_t n = read(c->fd, c->rbuf + c->rlen, RBUF - c->rlen);
        if (n > 0) { c->rlen += (int)n; continue; }
        if (n == 0) { close_conn(ep, c); return; }            /* peer closed     */
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_conn(ep, c); return;
    }
    if (process(ep, c) < 0) close_conn(ep, c);
}

static void warm_dataset(void) {
    volatile int sink = 0;
    const char *base = (const char *)g_ds.hdr;
    for (size_t i = 0; i < g_ds.map_len; i += 4096) sink += base[i];
    /* warm the hot code path with a dummy query per bucket */
    int16_t q[VLANES]; memset(q, 0, sizeof(q));
    for (int b = 0; b < NBUCKETS; b++) sink += knn_fraud_count_adaptive(&g_ds, q, b, g_nprobe);
    (void)sink;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <packed.bin> <port|unix-socket>\n", argv[0]); return 1; }
    const char *listen_arg = argv[2];
    int unix_listen = listen_arg[0] == '/';
    int port = unix_listen ? 0 : atoi(listen_arg);

    if (ds_open(&g_ds, argv[1]) != 0) return 1;
    { const char *np = getenv("NPROBE"); if (np && *np) g_nprobe = atoi(np); }
    fprintf(stderr, "nprobe=%d\n", g_nprobe);
    build_responses();
    warm_dataset();

    int lfd = socket(unix_listen ? AF_UNIX : AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    if (!unix_listen) {
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                                 .sin_port = htons((uint16_t)port) };
        if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    } else {
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", listen_arg);
        unlink(listen_arg);
        if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind unix"); return 1; }
    }
    if (listen(lfd, 1024) < 0) { perror("listen"); return 1; }

    int ep = epoll_create1(0);
    g_ep = ep;
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    if (unix_listen)
        fprintf(stderr, "api ready on %s (%u refs)\n", listen_arg, g_ds.nrefs);
    else
        fprintf(stderr, "api ready on :%d (%u refs)\n", port, g_ds.nrefs);

    struct epoll_event evs[MAXEV];
    for (;;) {
        int nf = epoll_wait(ep, evs, MAXEV, -1);
        for (int i = 0; i < nf; i++) {
            int fd = evs[i].data.fd;
            if (fd == lfd) {
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
                    if (cfd < 0) break;
                    if (cfd >= (1 << 16)) { close(cfd); continue; }
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    Conn *c = calloc(1, sizeof(Conn));
                    c->fd = cfd;
                    c->want = EPOLLIN | EPOLLET;
                    conns[cfd] = c;
                    struct epoll_event ce = { .events = EPOLLIN | EPOLLET, .data.fd = cfd };
                    epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ce);
                }
                continue;
            }
            Conn *c = conns[fd];
            if (!c) continue;
            if (evs[i].events & (EPOLLHUP | EPOLLERR)) { close_conn(ep, c); continue; }
            if (evs[i].events & EPOLLOUT) { if (flush_w(ep, c) < 0) { close_conn(ep, c); continue; } }
            if (evs[i].events & EPOLLIN)  on_readable(ep, c);
        }
    }
}
