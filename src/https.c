/* SPDX-License-Identifier: MPL-2.0 */
/* The catalog's HTTP(S) client: see https.h. */
#define _GNU_SOURCE
#include "https.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STALL_MS 30000u
#define CONNECT_MS 15000u
#define REDIRECTS 5
#define HEAD_MAX 65536
#define URL_MAX 8192   /* (GitHub's asset redirects carry a long signed query) */
#define CA_DIR_DEFAULT "/etc/ssl/certs"
#define CA_FILE_DEFAULT "/etc/ssl/certs/ca-certificates.crt"

/* OpenSSL 3's constants (ssl.h, tls1.h, x509_vfy.h: stable ABI values) */
#define SSL_VERIFY_PEER 1
#define SSL_ERROR_SSL 1
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define TLSEXT_NAMETYPE_host_name 0
#define TLS1_2_VERSION 0x0303
#define X509_V_OK 0

static char g_ca_dir[512];

/* ------------------------------------------------------------------ libssl, opened at run time --- */
static struct {
    int tried, ok;
    char why[200];
    void *ctx;   /* SSL_CTX: one, made once */
    const void *(*TLS_client_method)(void);
    void *(*SSL_CTX_new)(const void *);
    long (*SSL_CTX_ctrl)(void *, int, long, void *);
    int (*SSL_CTX_load_verify_locations)(void *, const char *, const char *);
    void (*SSL_CTX_set_verify)(void *, int, void *);
    void *(*SSL_new)(void *);
    void (*SSL_free)(void *);
    int (*SSL_set_fd)(void *, int);
    long (*SSL_ctrl)(void *, int, long, void *);
    int (*SSL_set1_host)(void *, const char *);
    void *(*SSL_get0_param)(void *);
    int (*SSL_connect)(void *);
    int (*SSL_read)(void *, void *, int);
    int (*SSL_write)(void *, const void *, int);
    int (*SSL_shutdown)(void *);
    int (*SSL_get_error)(const void *, int);
    long (*SSL_get_verify_result)(const void *);
    const char *(*X509_verify_cert_error_string)(long);
    int (*X509_VERIFY_PARAM_set1_ip_asc)(void *, const char *);
    unsigned long (*ERR_get_error)(void);
    void (*ERR_error_string_n)(unsigned long, char *, size_t);
    void (*ERR_clear_error)(void);
} T;
static pthread_mutex_t g_tls_mu = PTHREAD_MUTEX_INITIALIZER;

void https_ca_dir(const char *dir)
{
    snprintf(g_ca_dir, sizeof g_ca_dir, "%s", dir ? dir : "");
}

static void ssl_errors(char *out, int cap)
{
    unsigned long e, last = 0;
    while ((e = T.ERR_get_error()))
        last = e;
    if (last)
        T.ERR_error_string_n(last, out, (size_t)cap);
    else
        out[0] = 0;
}

/* libssl and libcrypto opened, the context made: 0, -1 (T.why) */
static int tls_init(void)
{
    void *ssl, *cry;
    const char *dir;
    int loaded = 0;
    pthread_mutex_lock(&g_tls_mu);
    if (T.tried) {
        pthread_mutex_unlock(&g_tls_mu);
        return T.ok ? 0 : -1;
    }
    T.tried = 1;
    {
        const char *libssl = "libssl.so.3";
#ifdef RFM14_TEST_BUILD
        if (getenv("RFM14_TEST_LIBSSL"))   /* (tests/dl_e2e.sh: a system without it) */
            libssl = getenv("RFM14_TEST_LIBSSL");
#endif
        if (!(cry = dlopen("libcrypto.so.3", RTLD_NOW | RTLD_LOCAL)) || !(ssl = dlopen(libssl, RTLD_NOW | RTLD_LOCAL))) {
            snprintf(T.why, sizeof T.why, "no TLS on this system (%s): https downloads are off", dlerror());
            pthread_mutex_unlock(&g_tls_mu);
            return -1;
        }
    }
#define SYM(lib, name)                                                                                            \
    if (!(*(void **)&T.name = dlsym(lib, #name))) {                                                              \
        snprintf(T.why, sizeof T.why, "the system's OpenSSL has no %s: https downloads are off", #name);          \
        pthread_mutex_unlock(&g_tls_mu);                                                                          \
        return -1;                                                                                                \
    }
    SYM(ssl, TLS_client_method) SYM(ssl, SSL_CTX_new) SYM(ssl, SSL_CTX_ctrl) SYM(ssl, SSL_CTX_load_verify_locations)
    SYM(ssl, SSL_CTX_set_verify) SYM(ssl, SSL_new) SYM(ssl, SSL_free) SYM(ssl, SSL_set_fd) SYM(ssl, SSL_ctrl)
    SYM(ssl, SSL_set1_host) SYM(ssl, SSL_get0_param) SYM(ssl, SSL_connect) SYM(ssl, SSL_read) SYM(ssl, SSL_write)
    SYM(ssl, SSL_shutdown) SYM(ssl, SSL_get_error) SYM(ssl, SSL_get_verify_result)
    SYM(cry, X509_verify_cert_error_string) SYM(cry, X509_VERIFY_PARAM_set1_ip_asc) SYM(cry, ERR_get_error)
    SYM(cry, ERR_error_string_n) SYM(cry, ERR_clear_error)
#undef SYM
    if (!(T.ctx = T.SSL_CTX_new(T.TLS_client_method()))) {
        snprintf(T.why, sizeof T.why, "SSL_CTX_new failed: https downloads are off");
        pthread_mutex_unlock(&g_tls_mu);
        return -1;
    }
    T.SSL_CTX_ctrl(T.ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, NULL);
    T.SSL_CTX_set_verify(T.ctx, SSL_VERIFY_PEER, NULL);
    dir = g_ca_dir[0] ? g_ca_dir : CA_DIR_DEFAULT;
    {
        struct stat sb;
        if (!g_ca_dir[0] && stat(CA_FILE_DEFAULT, &sb) == 0 &&
            T.SSL_CTX_load_verify_locations(T.ctx, CA_FILE_DEFAULT, NULL) == 1)
            loaded = 1;
        if (stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode) && T.SSL_CTX_load_verify_locations(T.ctx, NULL, dir) == 1)
            loaded = 1;
    }
    T.ERR_clear_error();
    if (!loaded) {
        snprintf(T.why, sizeof T.why, "no CA certificates in %s: https downloads are off", dir);
        pthread_mutex_unlock(&g_tls_mu);
        return -1;
    }
    T.ok = 1;
    fprintf(stderr, "https: the system's OpenSSL 3, CA certificates from %s\n", dir);
    pthread_mutex_unlock(&g_tls_mu);
    return 0;
}

/* ------------------------------------------------------------------ URLs --- */
typedef struct {
    int tls;
    char host[256], port[8], path[URL_MAX];
} url_t;

static int url_parse(const char *s, url_t *u, char *err, int cap)
{
    const char *h, *e, *p;
    size_t hl;
    memset(u, 0, sizeof *u);
    if (!strncmp(s, "https://", 8))
        u->tls = 1, h = s + 8;
    else if (!strncmp(s, "http://", 7))
        h = s + 7;
    else {
        snprintf(err, (size_t)cap, "%.200s: not an http(s) URL", s);
        return -1;
    }
    for (e = h; *e && *e != '/' && *e != ':' && *e != '?' && *e != '#'; e++)
        if (!((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9') || *e == '.' ||
              *e == '-')) {
            snprintf(err, (size_t)cap, "%.200s: a host name this client does not take", s);
            return -1;
        }
    hl = (size_t)(e - h);
    if (!hl || hl >= sizeof u->host) {
        snprintf(err, (size_t)cap, "%.200s: no host", s);
        return -1;
    }
    memcpy(u->host, h, hl);
    snprintf(u->port, sizeof u->port, "%s", u->tls ? "443" : "80");
    if (*e == ':') {
        long port;
        char *end;
        port = strtol(e + 1, &end, 10);
        if (end == e + 1 || port < 1 || port > 65535 || (*end && *end != '/' && *end != '?')) {
            snprintf(err, (size_t)cap, "%.200s: a bad port", s);
            return -1;
        }
        snprintf(u->port, sizeof u->port, "%ld", port);
        e = end;
    }
    p = *e == '/' ? e : "/";
    if (*e == '?')
        p = e;   /* (a query with no path: "/" + it) */
    if (strlen(p) + 2 >= sizeof u->path) {
        snprintf(err, (size_t)cap, "%.200s: too long", s);
        return -1;
    }
    snprintf(u->path, sizeof u->path, "%s%s", *p == '/' ? "" : "/", p);
    {   /* no fragment; nothing that would break the request line */
        char *f = strchr(u->path, '#');
        unsigned char *c;
        if (f)
            *f = 0;
        for (c = (unsigned char *)u->path; *c; c++)
            if (*c <= ' ' || *c >= 127) {
                snprintf(err, (size_t)cap, "%.200s: a character a URL may not have", s);
                return -1;
            }
    }
    return 0;
}

/* a Location against the URL it came from -> out */
static int url_join(const url_t *base, const char *loc, char *out, int cap)
{
    if (!strncmp(loc, "https://", 8) || !strncmp(loc, "http://", 7))
        return snprintf(out, (size_t)cap, "%s", loc) < cap ? 0 : -1;
    if (loc[0] == '/' && loc[1] == '/')
        return snprintf(out, (size_t)cap, "%s:%s", base->tls ? "https" : "http", loc) < cap ? 0 : -1;
    if (loc[0] == '/')
        return snprintf(out, (size_t)cap, "%s://%s:%s%s", base->tls ? "https" : "http", base->host, base->port, loc) <
                       cap ? 0 : -1;
    {   /* relative to the path's folder */
        const char *slash = strrchr(base->path, '/');
        const int dl = slash ? (int)(slash - base->path) + 1 : 1;
        return snprintf(out, (size_t)cap, "%s://%s:%s%.*s%s", base->tls ? "https" : "http", base->host, base->port, dl,
                        slash ? base->path : "/", loc) < cap ? 0 : -1;
    }
}

/* ------------------------------------------------------------------ the connection --- */
typedef struct {
    int fd;
    void *ssl;
    uint8_t buf[32768];
    int pos, len;
    volatile int *cancel;
    uint64_t last;      /* the last byte's time */
    char *err;
    int cap;
} conn_t;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int cancelled(conn_t *c)
{
    if (c->cancel && *c->cancel) {
        snprintf(c->err, (size_t)c->cap, "cancelled");
        return 1;
    }
    if (now_ms() - c->last > STALL_MS) {
        snprintf(c->err, (size_t)c->cap, "no data for %u s", STALL_MS / 1000u);
        return 1;
    }
    return 0;
}

static int tcp_connect(const url_t *u, conn_t *c)
{
    struct addrinfo hints, *res = NULL, *a;
    int rc;
    char why[160] = "no address";
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((rc = getaddrinfo(u->host, u->port, &hints, &res)) != 0) {
        snprintf(c->err, (size_t)c->cap, "%s: %s", u->host, gai_strerror(rc));
        return -1;
    }
    for (a = res; a; a = a->ai_next) {
        const int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        const uint64_t t0 = now_ms();
        int fl, ok = 0;
        if (fd < 0)
            continue;
        fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            ok = 1;
        else if (errno == EINPROGRESS) {
            for (;;) {
                struct pollfd pf = { fd, POLLOUT, 0 };
                const int r = poll(&pf, 1, 200);
                if (r > 0) {
                    int e = 0;
                    socklen_t el = sizeof e;
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
                    if (e)
                        snprintf(why, sizeof why, "%s", strerror(e));
                    else
                        ok = 1;
                    break;
                }
                if ((r < 0 && errno != EINTR) || (c->cancel && *c->cancel) || now_ms() - t0 > CONNECT_MS) {
                    snprintf(why, sizeof why, "%s", c->cancel && *c->cancel ? "cancelled" : "timed out");
                    break;
                }
            }
        } else
            snprintf(why, sizeof why, "%s", strerror(errno));
        if (ok) {
            struct timeval tv = { 0, 250000 };   /* (reads and writes wake up to look at cancel and the stall) */
            const int one = 1;
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            c->fd = fd;
            freeaddrinfo(res);
            return 0;
        }
        close(fd);
    }
    freeaddrinfo(res);
    snprintf(c->err, (size_t)c->cap, "%s:%s: cannot connect (%s)", u->host, u->port, why);
    return -1;
}

static int tls_start(const url_t *u, conn_t *c)
{
    struct in6_addr ip;
    const int is_ip = inet_pton(AF_INET, u->host, &ip) == 1 || inet_pton(AF_INET6, u->host, &ip) == 1;
    char e[200];
    if (tls_init() != 0) {
        snprintf(c->err, (size_t)c->cap, "%s", T.why);
        return -1;
    }
    T.ERR_clear_error();
    if (!(c->ssl = T.SSL_new(T.ctx)) || T.SSL_set_fd(c->ssl, c->fd) != 1) {
        snprintf(c->err, (size_t)c->cap, "SSL_new failed");
        return -1;
    }
    if (is_ip) {   /* (the certificate must name the address) */
        if (T.X509_VERIFY_PARAM_set1_ip_asc(T.SSL_get0_param(c->ssl), u->host) != 1) {
            snprintf(c->err, (size_t)c->cap, "%s: cannot check the certificate against it", u->host);
            return -1;
        }
    } else if (T.SSL_ctrl(c->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)u->host) != 1 ||
               T.SSL_set1_host(c->ssl, u->host) != 1) {
        snprintf(c->err, (size_t)c->cap, "%s: cannot check the certificate against it", u->host);
        return -1;
    }
    c->last = now_ms();
    for (;;) {
        const int r = T.SSL_connect(c->ssl);
        int se;
        if (r == 1)
            break;
        se = T.SSL_get_error(c->ssl, r);
        if ((se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) && !cancelled(c))
            continue;
        if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE)
            return -1;
        {
            const long v = T.SSL_get_verify_result(c->ssl);
            ssl_errors(e, sizeof e);
            if (v != X509_V_OK)
                snprintf(c->err, (size_t)c->cap, "%s: the certificate is refused: %s", u->host,
                         T.X509_verify_cert_error_string(v));
            else
                snprintf(c->err, (size_t)c->cap, "%s: TLS handshake failed (%s)", u->host,
                         e[0] ? e : se == SSL_ERROR_SYSCALL ? "connection closed" : "?");
        }
        return -1;
    }
    if (T.SSL_get_verify_result(c->ssl) != X509_V_OK) {   /* (cannot be, with SSL_VERIFY_PEER: said twice) */
        snprintf(c->err, (size_t)c->cap, "%s: the certificate is refused", u->host);
        return -1;
    }
    return 0;
}

static void conn_close(conn_t *c)
{
    if (c->ssl) {
        T.SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static int conn_write(conn_t *c, const char *b, int n)
{
    c->last = now_ms();
    while (n > 0) {
        int w;
        if (c->ssl) {
            w = T.SSL_write(c->ssl, b, n);
            if (w <= 0) {
                const int se = T.SSL_get_error(c->ssl, w);
                if ((se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) && !cancelled(c))
                    continue;
                if (!c->err[0])
                    snprintf(c->err, (size_t)c->cap, "cannot send the request");
                return -1;
            }
        } else {
            w = (int)send(c->fd, b, (size_t)n, MSG_NOSIGNAL);
            if (w < 0) {
                if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && !cancelled(c))
                    continue;
                if (!c->err[0])
                    snprintf(c->err, (size_t)c->cap, "cannot send the request: %s", strerror(errno));
                return -1;
            }
        }
        b += w, n -= w;
        c->last = now_ms();
    }
    return 0;
}

/* more bytes into buf: >0 how many, 0 the end, -1 an error (c->err) */
static int conn_fill(conn_t *c)
{
    if (c->pos > 0 && c->pos < c->len) {
        memmove(c->buf, c->buf + c->pos, (size_t)(c->len - c->pos));
        c->len -= c->pos;
        c->pos = 0;
    } else if (c->pos >= c->len)
        c->pos = c->len = 0;
    if (c->len >= (int)sizeof c->buf) {
        snprintf(c->err, (size_t)c->cap, "a line too long");
        return -1;
    }
    for (;;) {
        int r;
        if (cancelled(c))
            return -1;
        if (c->ssl) {
            r = T.SSL_read(c->ssl, c->buf + c->len, (int)sizeof c->buf - c->len);
            if (r <= 0) {
                const int se = T.SSL_get_error(c->ssl, r);
                if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE)
                    continue;
                if (se == SSL_ERROR_ZERO_RETURN)
                    return 0;
                if (se == SSL_ERROR_SYSCALL || se == SSL_ERROR_SSL) {   /* (a close with no close_notify: an end) */
                    char e[160];
                    ssl_errors(e, sizeof e);
                    if (se == SSL_ERROR_SYSCALL || strstr(e, "unexpected eof"))
                        return 0;
                    snprintf(c->err, (size_t)c->cap, "TLS: %s", e);
                    return -1;
                }
                snprintf(c->err, (size_t)c->cap, "TLS read failed");
                return -1;
            }
        } else {
            r = (int)recv(c->fd, c->buf + c->len, sizeof c->buf - (size_t)c->len, 0);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                snprintf(c->err, (size_t)c->cap, "read failed: %s", strerror(errno));
                return -1;
            }
            if (r == 0)
                return 0;
        }
        c->len += r;
        c->last = now_ms();
        return r;
    }
}

/* one line (CRLF or LF, without it) into out: 0, -1 */
static int conn_line(conn_t *c, char *out, int cap)
{
    for (;;) {
        int i;
        for (i = c->pos; i < c->len; i++)
            if (c->buf[i] == '\n') {
                int n = i - c->pos;
                if (n > 0 && c->buf[c->pos + n - 1] == '\r')
                    n--;
                if (n >= cap) {
                    snprintf(c->err, (size_t)c->cap, "a header line too long");
                    return -1;
                }
                memcpy(out, c->buf + c->pos, (size_t)n);
                out[n] = 0;
                c->pos = i + 1;
                return 0;
            }
        {
            const int r = conn_fill(c);
            if (r <= 0) {
                if (r == 0)
                    snprintf(c->err, (size_t)c->cap, "the server closed the connection early");
                return -1;
            }
        }
    }
}

/* ------------------------------------------------------------------ one GET --- */
typedef struct {
    FILE *f;
    uint64_t got, len, max;
    https_progress_fn progress;
    void *ctx;
} sink_t;

static int sink(conn_t *c, sink_t *s, const uint8_t *b, size_t n)
{
    if (s->got + n > s->max) {
        snprintf(c->err, (size_t)c->cap, "bigger than %llu bytes: refused", (unsigned long long)s->max);
        return -1;
    }
    if (n && fwrite(b, 1, n, s->f) != n) {
        snprintf(c->err, (size_t)c->cap, "cannot write the download: %s", strerror(errno));
        return -1;
    }
    s->got += n;
    if (s->progress)
        s->progress(s->ctx, s->got, s->len);
    return 0;
}

/* up to n body bytes (n < 0: to the end) */
static int body_copy(conn_t *c, sink_t *s, int64_t n)
{
    for (;;) {
        int avail = c->len - c->pos;
        if (n == 0)
            return 0;
        if (avail == 0) {
            const int r = conn_fill(c);
            if (r < 0)
                return -1;
            if (r == 0) {
                if (n < 0)
                    return 0;
                snprintf(c->err, (size_t)c->cap, "the server closed the connection early (%llu bytes)",
                         (unsigned long long)s->got);
                return -1;
            }
            avail = c->len - c->pos;
        }
        if (n > 0 && avail > n)
            avail = (int)n;
        if (sink(c, s, c->buf + c->pos, (size_t)avail) != 0)
            return -1;
        c->pos += avail;
        if (n > 0)
            n -= avail;
    }
}

/* one request: 0 the body is in s; 1 a redirect (loc); -1 err */
static int get_once(const url_t *u, sink_t *s, volatile int *cancel, char *loc, int lcap, char *err, int cap)
{
    conn_t *c = calloc(1, sizeof *c);
    char line[16384], req[URL_MAX + 512];
    int status = 0, chunked = 0, rc = -1, n;
    int64_t clen = -1;
    if (!c) {
        snprintf(err, (size_t)cap, "out of memory");
        return -1;
    }
    c->fd = -1, c->cancel = cancel, c->err = err, c->cap = cap;
    err[0] = 0;
    loc[0] = 0;
    if (tcp_connect(u, c) != 0 || (u->tls && tls_start(u, c) != 0))
        goto out;
    n = snprintf(req, sizeof req,
                 "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: RemoteFM1-Move/1\r\nAccept: */*\r\n"
                 "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                 u->path, u->host, strcmp(u->port, u->tls ? "443" : "80") ? ":" : "",
                 strcmp(u->port, u->tls ? "443" : "80") ? u->port : "");
    c->last = now_ms();
    if (conn_write(c, req, n) != 0 || conn_line(c, line, sizeof line) != 0)
        goto out;
    if (strncmp(line, "HTTP/1.", 7) || sscanf(line + 8, "%d", &status) != 1) {
        snprintf(err, (size_t)cap, "%s: not an HTTP answer", u->host);
        goto out;
    }
    {   /* the headers */
        int total = 0;
        for (;;) {
            char *v;
            if (conn_line(c, line, sizeof line) != 0)
                goto out;
            if (!line[0])
                break;
            if ((total += (int)strlen(line)) > HEAD_MAX) {
                snprintf(err, (size_t)cap, "%s: headers too long", u->host);
                goto out;
            }
            if (!(v = strchr(line, ':')))
                continue;
            *v++ = 0;
            while (*v == ' ' || *v == '\t')
                v++;
            if (!strcasecmp(line, "Content-Length")) {
                char *end;
                const long long x = strtoll(v, &end, 10);
                if (end == v || x < 0) {
                    snprintf(err, (size_t)cap, "%s: a bad Content-Length", u->host);
                    goto out;
                }
                clen = x;
            } else if (!strcasecmp(line, "Transfer-Encoding"))
                chunked = strcasestr(v, "chunked") != NULL;
            else if (!strcasecmp(line, "Location"))
                snprintf(loc, (size_t)lcap, "%s", v);
        }
    }
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        if (!loc[0]) {
            snprintf(err, (size_t)cap, "%s: a redirect with no Location", u->host);
            goto out;
        }
        rc = 1;
        goto out;
    }
    if (status != 200) {
        snprintf(err, (size_t)cap, "%s%s: HTTP %d", u->host, u->path, status);
        goto out;
    }
    if (chunked) {
        for (;;) {
            unsigned long long sz;
            char *end;
            if (conn_line(c, line, sizeof line) != 0)
                goto out;
            sz = strtoull(line, &end, 16);
            if (end == line) {
                snprintf(err, (size_t)cap, "%s: a damaged chunk", u->host);
                goto out;
            }
            if (sz == 0)
                break;
            if (sz > s->max || body_copy(c, s, (int64_t)sz) != 0) {
                if (!err[0])
                    snprintf(err, (size_t)cap, "bigger than %llu bytes: refused", (unsigned long long)s->max);
                goto out;
            }
            if (conn_line(c, line, sizeof line) != 0)   /* (the chunk's CRLF) */
                goto out;
        }
    } else if (clen >= 0) {
        if ((uint64_t)clen > s->max) {
            snprintf(err, (size_t)cap, "%llu bytes, more than %llu: refused", (unsigned long long)clen,
                     (unsigned long long)s->max);
            goto out;
        }
        s->len = (uint64_t)clen;
        if (body_copy(c, s, clen) != 0)
            goto out;
    } else if (body_copy(c, s, -1) != 0)
        goto out;
    rc = 0;
out:
    conn_close(c);
    free(c);
    return rc;
}

int https_get(const char *url, const char *out, uint64_t max, int allow_http, https_progress_fn progress, void *ctx,
              volatile int *cancel, char *err, int cap)
{
    char cur[URL_MAX], loc[URL_MAX];
    int hop, rc = -1, was_tls = 0;
    sink_t s;
    memset(&s, 0, sizeof s);
    s.max = max, s.progress = progress, s.ctx = ctx;
    snprintf(cur, sizeof cur, "%s", url);
    for (hop = 0; hop <= REDIRECTS; hop++) {
        url_t u;
        if (url_parse(cur, &u, err, cap) != 0)
            return -1;
        if (!u.tls && (!allow_http || was_tls)) {
            snprintf(err, (size_t)cap, "%.200s: refused: %s", cur,
                     was_tls ? "a redirect from https to plain http" : "plain http (only https is used)");
            return -1;
        }
        was_tls |= u.tls;
        if (!(s.f = fopen(out, "wb"))) {
            snprintf(err, (size_t)cap, "cannot write %s: %s", out, strerror(errno));
            return -1;
        }
        s.got = s.len = 0;
        rc = get_once(&u, &s, cancel, loc, sizeof loc, err, cap);
        if (fclose(s.f) != 0 && rc == 0) {
            snprintf(err, (size_t)cap, "cannot write %s: %s", out, strerror(errno));
            rc = -1;
        }
        if (rc != 1)
            break;
        if (url_join(&u, loc, cur, sizeof cur) != 0) {
            snprintf(err, (size_t)cap, "%s: a redirect too long", u.host);
            rc = -1;
            break;
        }
        rc = -1;
        snprintf(err, (size_t)cap, "%.200s: more than %d redirects", url, REDIRECTS);
    }
    if (rc != 0)
        unlink(out);
    return rc;
}
