/* lb.c — dumb round-robin TCP load balancer (epoll, no payload inspection).
 *
 * Listens on :PORT, and for every accepted client connection picks the next
 * backend in strict round-robin (atomic counter), opens a TCP connection to it,
 * and relays bytes both ways with backpressure. It never parses the payload —
 * compliant with "the load balancer cannot apply detection logic".
 *
 * Usage: lb <listen_port> <host1> <port1> <host2> <port2>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>

#define MAXEV 256
/* Per connection we hold two relay buffers (c->b and b->c), so RAM scales as
 * 2*BUF*concurrent_conns. The fraud request/response are tiny (~500B / ~80B), so
 * a small buffer is plenty; 64KB here let ~250 keep-alive conns blow past the
 * LB's 24MB cgroup and get it OOM-killed (exit 137) under load — which fails ALL
 * traffic. 8KB keeps 2*8KB*~250 ≈ 4MB, with headroom for connection spikes. */
#define BUF   8192
#define NBK   2

static struct sockaddr_storage g_backend[NBK];
static socklen_t g_backend_len[NBK];
static unsigned g_rr = 0;

typedef struct { char buf[BUF]; int len, off; } Half;
typedef struct {
    int  cfd, bfd;
    int  bconnected;
    Half c2b;          /* client -> backend */
    Half b2c;          /* backend -> client */
} Conn;

typedef struct { Conn *c; int is_client; int want; } Slot;
static Slot slots[1 << 16];

static void arm(int ep, int fd, int want) {
    if (slots[fd].want == want) return;
    slots[fd].want = want;
    struct epoll_event ev = { .events = (uint32_t)want, .data.fd = fd };
    epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

static void conn_close(int ep, Conn *c) {
    if (c->cfd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, c->cfd, NULL); close(c->cfd); slots[c->cfd].c = NULL; }
    if (c->bfd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, c->bfd, NULL); close(c->bfd); slots[c->bfd].c = NULL; }
    free(c);
}

/* Flush h to out_fd; manage EPOLLIN/EPOLLOUT on in/out. Returns -1 to close. */
static int pump_write(int ep, Half *h, int out_fd, int in_fd) {
    while (h->off < h->len) {
        ssize_t w = write(out_fd, h->buf + h->off, h->len - h->off);
        if (w > 0) { h->off += (int)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            arm(ep, out_fd, EPOLLOUT);
            arm(ep, in_fd, 0);          /* stop reading until drained */
            return 0;
        }
        return -1;
    }
    h->off = h->len = 0;
    arm(ep, out_fd, EPOLLIN);
    arm(ep, in_fd, EPOLLIN);
    return 0;
}

static int pump_read(int ep, Half *h, int in_fd, int out_fd) {
    if (h->off < h->len) return 0;             /* still draining; don't read   */
    ssize_t n = read(in_fd, h->buf, BUF);
    if (n == 0) return -1;                      /* peer closed                  */
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    h->len = (int)n; h->off = 0;
    return pump_write(ep, h, out_fd, in_fd);
}

static void on_accept(int ep, int lfd) {
    for (;;) {
        int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
        if (cfd < 0) break;
        if (cfd >= (1 << 16)) { close(cfd); continue; }

        unsigned idx = __atomic_fetch_add(&g_rr, 1, __ATOMIC_RELAXED) % NBK;
        int family = g_backend[idx].ss_family;
        int bfd = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (bfd < 0 || bfd >= (1 << 16)) { if (bfd >= 0) close(bfd); close(cfd); continue; }

        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (family == AF_INET) setsockopt(bfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        int r = connect(bfd, (struct sockaddr *)&g_backend[idx], g_backend_len[idx]);
        if (r < 0 && errno != EINPROGRESS) { close(bfd); close(cfd); continue; }

        Conn *c = calloc(1, sizeof(Conn));
        c->cfd = cfd; c->bfd = bfd; c->bconnected = 0;
        slots[cfd] = (Slot){ .c = c, .is_client = 1, .want = 0 };
        slots[bfd] = (Slot){ .c = c, .is_client = 0, .want = EPOLLOUT };

        struct epoll_event eb = { .events = EPOLLOUT, .data.fd = bfd };  /* wait for connect */
        epoll_ctl(ep, EPOLL_CTL_ADD, bfd, &eb);
        struct epoll_event ec = { .events = 0, .data.fd = cfd };         /* idle until connected */
        epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ec);
    }
}

int main(int argc, char **argv) {
    if (argc != 4 && argc != 6) {
        fprintf(stderr, "usage: %s <port> <sock1> <sock2> | <port> <h1> <p1> <h2> <p2>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    if (argc == 4) {
        for (int i = 0; i < NBK; i++) {
            const char *path = argv[2 + i];
            struct sockaddr_un *u = (struct sockaddr_un *)&g_backend[i];
            memset(u, 0, sizeof(*u));
            u->sun_family = AF_UNIX;
            snprintf(u->sun_path, sizeof(u->sun_path), "%s", path);
            g_backend_len[i] = sizeof(*u);
        }
    } else {
        for (int i = 0; i < NBK; i++) {
            const char *host = argv[2 + i*2], *prt = argv[3 + i*2];
            struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
            /* retry DNS: backend service may not be up yet */
            int rc = -1;
            for (int t = 0; t < 60; t++) {
                rc = getaddrinfo(host, prt, &hints, &res);
                if (rc == 0) break;
                sleep(1);
            }
            if (rc != 0) { fprintf(stderr, "resolve %s:%s failed\n", host, prt); return 1; }
            memcpy(&g_backend[i], res->ai_addr, sizeof(struct sockaddr_in));
            g_backend_len[i] = (socklen_t)res->ai_addrlen;
            freeaddrinfo(res);
        }
    }

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons((uint16_t)port) };
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 1024) < 0) { perror("listen"); return 1; }

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
    if (argc == 4)
        fprintf(stderr, "lb listening on :%d -> %s, %s\n", port, argv[2], argv[3]);
    else
        fprintf(stderr, "lb listening on :%d -> %s:%s, %s:%s\n", port, argv[2], argv[3], argv[4], argv[5]);

    struct epoll_event evs[MAXEV];
    for (;;) {
        int nf = epoll_wait(ep, evs, MAXEV, -1);
        for (int i = 0; i < nf; i++) {
            int fd = evs[i].data.fd;
            uint32_t e = evs[i].events;
            if (fd == lfd) { on_accept(ep, lfd); continue; }

            Slot *s = &slots[fd];
            Conn *c = s->c;
            if (!c) continue;

            /* backend connect completion */
            if (!c->bconnected && fd == c->bfd && (e & EPOLLOUT)) {
                int err = 0; socklen_t sl = sizeof(err);
                getsockopt(c->bfd, SOL_SOCKET, SO_ERROR, &err, &sl);
                if (err != 0) { conn_close(ep, c); continue; }
                c->bconnected = 1;
                slots[c->bfd].want = -1; arm(ep, c->bfd, EPOLLIN);
                slots[c->cfd].want = -1; arm(ep, c->cfd, EPOLLIN);
                /* fall through: client may already have data, handled on its own event */
                continue;
            }

            if (e & (EPOLLHUP | EPOLLERR)) { conn_close(ep, c); continue; }

            int rc = 0;
            if (s->is_client) {
                if (e & EPOLLOUT) rc = pump_write(ep, &c->b2c, c->cfd, c->bfd);
                if (rc == 0 && (e & EPOLLIN)) rc = pump_read(ep, &c->c2b, c->cfd, c->bfd);
            } else {
                if (e & EPOLLOUT) rc = pump_write(ep, &c->c2b, c->bfd, c->cfd);
                if (rc == 0 && (e & EPOLLIN)) rc = pump_read(ep, &c->b2c, c->bfd, c->cfd);
            }
            if (rc < 0) conn_close(ep, c);
        }
    }
}
