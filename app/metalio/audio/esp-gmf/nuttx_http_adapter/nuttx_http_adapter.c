/*
 * nuttx_http_adapter.c — Implementation of the shim esp_http_client API
 * for NuttX / OpenVela.  This file is compiled alongside the gmf_io sources.
 *
 * Implementation notes
 * --------------------
 *  - Uses raw POSIX sockets.  Only http:// URLs are supported.
 *  - Supports HTTP/1.1 GET and POST with explicit Content-Length.
 *  - Supports "Range: bytes=N-" requests (used by gmf_io_http.c after seek).
 *  - Emits HTTP_EVENT_ON_HEADER for every response header before the body
 *    starts.  gmf_io_http.c uses this only for Content-Encoding: gzip.
 *  - Handles both plain Content-Length bodies and Transfer-Encoding: chunked.
 *  - HTTP redirects (301/302) are handled by the caller (gmf_io_http.c)
 *    calling esp_http_client_set_redirection() followed by
 *    esp_http_client_open() again, so we don't auto-follow redirects here.
 *  - The NuttX DNS client (CONFIG_NETDB_DNSCLIENT=y) is used via getaddrinfo().
 *
 * The file is deliberately small and self-contained.  Future TLS support can
 * be layered on by wrapping connect/read/write with a tls_ops struct (similar
 * to how NuttX's netutils/webclient does it).
 */

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_gmf_err.h"

/* -------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* -------------------------------------------------------------------------- */

#define NHA_HTTP_PORT            80
#define NHA_HEADER_MAX_LINE      1024   /* one response-header line */
#define NHA_REQUEST_MIN_SEND     16     /* keep send() spinning while small */

static const char *TAG = "nha";

/* -------------------------------------------------------------------------- */
/*  Debug logging helper.  snprintf() returns the *full* length even when it
 *  truncates, so writing `n` bytes from a small stack buffer would read past
 *  the end and spew garbage.  Clamp to the buffer size.                     */
/* -------------------------------------------------------------------------- */

static void nha_dbg(const char *fmt, ...)
{
    char dbg[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dbg, sizeof(dbg), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(dbg)) n = (int)sizeof(dbg) - 1;
    write(1, dbg, (size_t)n);
}

/* -------------------------------------------------------------------------- */
/*  Internal header list                                                      */
/* -------------------------------------------------------------------------- */

typedef struct nha_header_s {
    char                      *key;
    char                      *value;
    struct nha_header_s       *next;
} nha_header_t;

/* -------------------------------------------------------------------------- */
/*  Per-URL parsed components                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool         is_https;   /* only http:// is implemented */
    char        *host;       /* owned */
    int          port;
    char        *path_query; /* owned, includes leading '/' plus optional '?...' */
} nha_url_t;

/* -------------------------------------------------------------------------- */
/*  The real client context.                                                  */
/* -------------------------------------------------------------------------- */

struct esp_http_client {
    /* Configuration snapshot */
    esp_http_client_config_t   cfg;

    /* Current URL (possibly overridden via set_url) */
    char                      *url;
    nha_url_t                  parsed;

    /* Custom headers set via set_header / delete_header */
    nha_header_t              *headers;

    /* POST body, set via set_post_field (not really used by gmf_io) */
    char                      *post_body;
    int                        post_len;

    /* Socket state */
    int                        fd;          /* -1 if disconnected */
    bool                       connected;

    /* Request direction inferred from write_len passed to open() */
    bool                       is_writer;   /* !is_writer = reader */
    int                        content_written; /* bytes of body already sent */
    int                        write_total;   /* total body to write, -1 = unknown */

    /* Response state populated by fetch_headers */
    int                        status_code;
    int64_t                    content_length;    /* -1 if chunked / unknown */
    bool                       chunked;
    int64_t                    body_remaining;    /* for content-length reads */
    int64_t                    body_read;         /* read-call counter for throttled debug */

    /* Per-response buffered line read support (for chunked encoding) */
    char                       line_buf[NHA_HEADER_MAX_LINE];
    int                        line_len;
    int                        line_pos;

    /* Chunked state */
    int64_t                    chunk_remaining;   /* bytes left in current chunk */
    bool                       chunk_done;

    /* Stored errno for get_errno() */
    int                        last_errno;

    /* Redirect handling: caller's set_redirection() triggers reuse of
     * Location header value on the next open().  We store the raw string. */
    char                      *redirect_target;
};

/* -------------------------------------------------------------------------- */
/*  Tiny URL parser.                                                          */
/* -------------------------------------------------------------------------- */

static void nha_free_url(nha_url_t *u)
{
    free(u->host); u->host = NULL;
    free(u->path_query); u->path_query = NULL;
}

/* Returns 0 on success, -1 on failure (invalid URL). */
static int nha_parse_url(const char *s, nha_url_t *out)
{
    memset(out, 0, sizeof(*out));
    out->port = NHA_HTTP_PORT;
    if (!s) return -1;

    const char *p = s;
    /* scheme */
    if (!strncasecmp(p, "http://", 7)) {
        out->is_https = false; p += 7;
    } else if (!strncasecmp(p, "https://", 8)) {
        out->is_https = true; p += 8;
    } else {
        return -1;
    }
    /* host[:port][/path] */
    const char *slash = strchr(p, '/');
    const char *col   = strchr(p, ':');
    size_t host_len;
    if (col && (!slash || col < slash)) {
        host_len = (size_t)(col - p);
        out->port = atoi(col + 1);
    } else {
        host_len = slash ? (size_t)(slash - p) : strlen(p);
    }
    if (host_len == 0) return -1;
    out->host = (char *)malloc(host_len + 1);
    if (!out->host) return -1;
    memcpy(out->host, p, host_len); out->host[host_len] = '\0';

    if (slash) {
        out->path_query = strdup(slash);
    } else {
        out->path_query = strdup("/");
    }
    if (!out->path_query) return -1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Header list helpers.                                                      */
/* -------------------------------------------------------------------------- */

static void nha_free_headers(nha_header_t *h)
{
    while (h) {
        nha_header_t *n = h->next;
        free(h->key); free(h->value); free(h);
        h = n;
    }
}

static void nha_set_header(nha_header_t **list, const char *key, const char *value)
{
    /* Replace existing key. */
    for (nha_header_t *h = *list; h; h = h->next) {
        if (!strcasecmp(h->key, key)) {
            free(h->value);
            h->value = value ? strdup(value) : NULL;
            return;
        }
    }
    nha_header_t *h = (nha_header_t *)calloc(1, sizeof(*h));
    if (!h) return;
    h->key   = strdup(key);
    h->value = value ? strdup(value) : NULL;
    h->next  = *list;
    *list    = h;
}

static void nha_delete_header(nha_header_t **list, const char *key)
{
    nha_header_t **prev = list;
    for (nha_header_t *h = *list; h; prev = &h->next, h = h->next) {
        if (!strcasecmp(h->key, key)) {
            *prev = h->next;
            free(h->key); free(h->value); free(h);
            return;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  DNS + TCP connect.                                                        */
/* -------------------------------------------------------------------------- */

static int nha_connect_to_host(struct esp_http_client *c)
{
    if (c->parsed.is_https) {
        /* Not implemented.  We could layer OpenSSL/TinySocket later via ops. */
        c->last_errno = ENOTSUP;
        return -1;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", c->parsed.port);
    struct addrinfo *res = NULL;
    nha_dbg("NHA_DNS0 host=%s port=%s\n", c->parsed.host, port_str);
    int rc = getaddrinfo(c->parsed.host, port_str, &hints, &res);
    if (rc != 0) {
        nha_dbg("NHA_DNS_FAIL host=%s rc=%d\n", c->parsed.host, rc);
        ESP_LOGE(TAG, "getaddrinfo(%s) failed: %d", c->parsed.host, rc);
        c->last_errno = EIO;
        return -1;
    }
    if (res != NULL && res->ai_addr != NULL &&
        res->ai_addr->sa_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        nha_dbg("NHA_DNS_OK host=%s ip=%s\n",
                c->parsed.host, inet_ntoa(sa->sin_addr));
    }
    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        nha_dbg("NHA_CONN_FAIL host=%s errno=%d\n", c->parsed.host, errno);
        ESP_LOGE(TAG, "connect(%s:%d) failed (%s)", c->parsed.host, c->parsed.port,
                 strerror(errno));
        c->last_errno = errno;
        return -1;
    }
    nha_dbg("NHA_CONN_OK host=%s fd=%d\n", c->parsed.host, fd);

    /* Nagle holds a sub-MSS HTTP request until an ACK; radio playlists are
     * small GETs so the request would sit unsent while recv waits. */
    {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    /* Apply timeout_ms via SO_SNDTIMEO / SO_RCVTIMEO if configured.
     * Also used as a fallback; the actual recv path prefers poll() because
     * SO_RCVTIMEO is not reliable on this board's WiFi sockets. */
    if (c->cfg.timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec  = c->cfg.timeout_ms / 1000;
        tv.tv_usec = (c->cfg.timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    c->fd = fd;
    c->connected = true;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Socket send_all / recv helpers.                                           */
/* -------------------------------------------------------------------------- */

static int nha_send_all(int fd, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = send(fd, buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += n;
    }
    return total;
}

static void nha_close_sock(struct esp_http_client *c)
{
    if (!c || c->fd < 0) {
        return;
    }
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    c->fd = -1;
    c->connected = false;
}

static int nha_timeout_ms(struct esp_http_client *c)
{
    if (c && c->cfg.timeout_ms > 0) {
        return c->cfg.timeout_ms;
    }
    return 15000;
}

/* Blocking recv() after a successful send hangs on this port's WiFi TCP
 * (SO_RCVTIMEO is ignored by the driver). poll() is how MQTT/OTA wait. */
static int nha_recv(struct esp_http_client *c, char *buf, int want)
{
    if (!c || c->fd < 0 || !buf || want <= 0) {
        errno = EINVAL;
        return -1;
    }
    const int to = nha_timeout_ms(c);
    for (;;) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = c->fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, to);
        if (pr == 0) {
            errno = ETIMEDOUT;
            c->last_errno = ETIMEDOUT;
            nha_dbg("NHA_RX_TO fd=%d to=%d\n", c->fd, to);
            return -1;
        }
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            c->last_errno = errno;
            nha_dbg("NHA_POLL_ERR %d\n", errno);
            return -1;
        }
        int n = recv(c->fd, buf, want, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            c->last_errno = errno;
            nha_dbg("NHA_RX_ERR %d\n", errno);
        }
        return n;
    }
}

/* Read exactly up to \n inclusive (return line bytes including \n, 0 on EOF,
 * -1 on error).  Buffer is always NUL-terminated after the line. */
static int nha_read_line(struct esp_http_client *c, char *out, int out_max)
{
    int written = 0;
    /* First drain any buffered bytes from the last line-read. */
    while (c->line_pos < c->line_len) {
        char ch = c->line_buf[c->line_pos++];
        if (written + 2 > out_max) { out[written] = 0; return written; }
        out[written++] = ch;
        if (ch == '\n') { out[written] = 0; return written; }
    }
    /* Then read socket into the line buffer. */
    for (;;) {
        int n = nha_recv(c, c->line_buf, (int)sizeof(c->line_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            c->last_errno = errno;
            return -1;
        }
        if (n == 0) { out[written] = 0; return written; /* EOF */ }
        c->line_len = n;
        c->line_pos = 0;
        for (int i = 0; i < n; ++i) {
            char ch = c->line_buf[c->line_pos++];
            if (written + 2 > out_max) { out[written] = 0; return written; }
            out[written++] = ch;
            if (ch == '\n') { out[written] = 0; return written; }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Send request line + headers.                                              */
/* -------------------------------------------------------------------------- */

static int nha_send_request(struct esp_http_client *c)
{
    char tmp[256];
    /* Build in a dynamically grown buffer so large header lists fit. */
    size_t cap = 1024;
    size_t len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) return -1;
    #define appendf(...) do {                                              \
        int _n = snprintf(buf + len, cap - len, __VA_ARGS__);              \
        if (_n < 0) { free(buf); return -1; }                              \
        if ((size_t)_n >= cap - len) {                                     \
            size_t _new = cap * 2 + (size_t)_n;                            \
            char *_b = (char *)realloc(buf, _new);                         \
            if (!_b) { free(buf); return -1; }                             \
            buf = _b; cap = _new;                                          \
            _n = snprintf(buf + len, cap - len, __VA_ARGS__);              \
            if (_n < 0 || (size_t)_n >= cap - len) { free(buf); return -1;}\
        }                                                                  \
        len += (size_t)_n;                                                 \
    } while (0)

    appendf("%s %s HTTP/1.1\r\n",
            c->is_writer ? "POST" : "GET", c->parsed.path_query);
    appendf("Host: %s", c->parsed.host);
    if (c->parsed.port != NHA_HTTP_PORT) {
        len -= 2; /* drop "\r\n" temporarily */
        appendf(":%d", c->parsed.port);
    }
    appendf("\r\n");
    appendf("User-Agent: metalio-gmf-http/1.0\r\n");
    appendf("Accept: */*\r\n");

    if (c->is_writer) {
        /* If write_total is known (>=0) send Content-Length, else chunked. */
        if (c->write_total >= 0) {
            appendf("Content-Length: %d\r\n", c->write_total);
        } else {
            appendf("Transfer-Encoding: chunked\r\n");
        }
        appendf("Content-Type: application/octet-stream\r\n");
    }

    /* Custom headers (including Range). */
    for (nha_header_t *h = c->headers; h; h = h->next) {
        if (h->key && h->value) {
            appendf("%s: %s\r\n", h->key, h->value);
        }
    }
    appendf("Connection: close\r\n");
    appendf("\r\n");
    #undef appendf

    int rc = nha_send_all(c->fd, buf, (int)len);
    nha_dbg("NHA_SENT %d/%d\n", rc, (int)len);
    free(buf);
    if (rc < 0) {
        c->last_errno = errno;
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Emit one HTTP_EVENT_ON_HEADER callback.                                   */
/* -------------------------------------------------------------------------- */

static void nha_emit_header_cb(struct esp_http_client *c,
                                const char *k, const char *v)
{
    if (!c->cfg.event_handler) return;
    esp_http_client_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.client      = c;
    evt.user_data   = c->cfg.user_data;
    evt.event_id    = HTTP_EVENT_ON_HEADER;
    evt.header_key  = k;
    evt.header_value = v;
    (void)c->cfg.event_handler(&evt);
}

/* -------------------------------------------------------------------------- */
/*  Parse response status + headers.  Returns content-length or -1 if none.   */
/*  On redirect 301/302, stores the Location target in redirect_target.      */
/* -------------------------------------------------------------------------- */

static int64_t nha_parse_headers(struct esp_http_client *c)
{
    write(1, "NHA_FETCH0\n", 11);
    c->status_code    = 0;
    c->content_length = -1;
    c->chunked        = false;
    c->body_remaining = 0;
    c->chunk_remaining = 0;
    c->chunk_done     = false;
    free(c->redirect_target); c->redirect_target = NULL;

    char line[NHA_HEADER_MAX_LINE];

    /* Status line. */
    int n = nha_read_line(c, line, sizeof(line));
    if (n <= 0) return -1;
    /* "HTTP/X.Y NNN reason" */
    if (!strncasecmp(line, "HTTP/", 5)) {
        const char *p = line + 5;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        c->status_code = atoi(p);
    }

    /* Header lines until empty line. */
    for (;;) {
        n = nha_read_line(c, line, sizeof(line));
        if (n <= 0) break;
        /* Strip trailing \r\n */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n == 0) break;

        /* Split at first ':' */
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *val = colon + 1;
        while (*val == ' ') val++;

        nha_emit_header_cb(c, line, val);

        if (!strcasecmp(line, "Content-Length")) {
            c->content_length = (int64_t)atoll(val);
            c->body_remaining = c->content_length;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            if (strcasestr(val, "chunked")) {
                c->chunked = true;
                c->content_length = -1;
            }
        } else if (!strcasecmp(line, "Location")) {
            free(c->redirect_target);
            c->redirect_target = strdup(val);
        }
    }
    nha_dbg("NHA_HDR status=%d cl=%lld chunked=%d\n",
            c->status_code, (long long)c->content_length, c->chunked);
    return c->content_length;
}

/* -------------------------------------------------------------------------- */
/*  Chunked read helper: read one chunk size line, return remaining bytes.    */
/* -------------------------------------------------------------------------- */

static int64_t nha_read_chunk_size(struct esp_http_client *c)
{
    char line[NHA_HEADER_MAX_LINE];
    int n = nha_read_line(c, line, sizeof(line));
    if (n <= 0) return -1;
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
    if (n == 0) return 0; /* final */
    /* Parse hex, ignore any extension after ';'. */
    char *semi = strchr(line, ';'); if (semi) *semi = 0;
    int64_t sz = 0;
    for (int i = 0; line[i]; ++i) {
        char ch = (char)tolower((unsigned char)line[i]);
        int v = (ch >= '0' && ch <= '9') ? (ch - '0') :
                (ch >= 'a' && ch <= 'f') ? (10 + ch - 'a') : -1;
        if (v < 0) break;
        sz = (sz << 4) | (int64_t)v;
    }
    return sz;
}

/* ========================================================================== */
/*                          PUBLIC API IMPLEMENTATION                         */
/* ========================================================================== */

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    if (!cfg) { return NULL; }
    struct esp_http_client *c =
        (struct esp_http_client *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg        = *cfg;
    c->fd         = -1;
    c->status_code= 0;
    c->content_length = -1;
    if (cfg->url) {
        c->url = strdup(cfg->url);
        if (!c->url || nha_parse_url(cfg->url, &c->parsed) != 0) {
            free(c->url); free(c);
            return NULL;
        }
    }
    return c;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (client->fd >= 0) {
        nha_close_sock(client);
    }
    free(client->url);
    free(client->post_body);
    free(client->redirect_target);
    nha_free_headers(client->headers);
    nha_free_url(&client->parsed);
    free(client);
    return ESP_OK;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url)
{
    if (!c || !url) return ESP_ERR_INVALID_ARG;
    /* A new URL may point to a different host.  Drop any existing TCP
     * connection so the next esp_http_client_open() establishes a fresh
     * socket.  The shim always sends "Connection: close", so reusing a stale
     * fd would make nha_send_request() hit a dead socket and fail. */
    if (c->fd >= 0) {
        nha_close_sock(c);
    }
    free(c->url);
    nha_free_url(&c->parsed);
    c->url = strdup(url);
    if (!c->url) return ESP_ERR_NO_MEM;
    if (nha_parse_url(url, &c->parsed) != 0) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,
                                     const char *key, const char *val)
{
    if (!c || !key) return ESP_ERR_INVALID_ARG;
    nha_set_header(&c->headers, key, val);
    return ESP_OK;
}

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *key)
{
    if (!c || !key) return ESP_ERR_INVALID_ARG;
    nha_delete_header(&c->headers, key);
    return ESP_OK;
}

int esp_http_client_get_post_field(esp_http_client_handle_t c, char **data)
{
    if (!c || !data) return -1;
    *data = c->post_body;
    return c->post_len;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,
                                          const char *data, int len)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    free(c->post_body);
    if (data && len > 0) {
        c->post_body = (char *)malloc((size_t)len);
        if (!c->post_body) return ESP_ERR_NO_MEM;
        memcpy(c->post_body, data, (size_t)len);
        c->post_len = len;
    } else {
        c->post_body = NULL;
        c->post_len  = 0;
    }
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    c->last_errno = 0;
    nha_dbg("NHA_OPEN0 url=%s\n", c->url ? c->url : "(null)");

    /* If a redirect target has been stored (set_redirection was called),
     * transparently replace the URL now so the next request uses it.
     * esp_http_client_set_url() also drops the stale fd, so the next
     * nha_connect_to_host() opens a fresh TCP connection. */
    if (c->redirect_target) {
        char *t = c->redirect_target;
        c->redirect_target = NULL;
        esp_err_t r = esp_http_client_set_url(c, t);
        free(t);
        if (r != ESP_OK) return ESP_FAIL;
    }

    c->is_writer         = (write_len != 0) ? true : false;
    c->write_total       = write_len;      /* -1 = unknown (POST no CL) */
    c->content_written   = 0;
    c->line_len          = 0;
    c->line_pos          = 0;
    c->body_read         = 0;

    if (c->fd < 0) {
        if (nha_connect_to_host(c) != 0) return ESP_FAIL;
    }
    if (nha_send_request(c) != 0) return ESP_FAIL;
    write(1, "NHA_REQ_OK\n", 11);

    if (c->cfg.event_handler) {
        esp_http_client_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.client    = c;
        evt.user_data = c->cfg.user_data;
        evt.event_id  = HTTP_EVENT_HEADERS_SENT;
        (void)c->cfg.event_handler(&evt);
    }
    return ESP_OK;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (c->fd >= 0) {
        /* Graceful close: for writer streams, send the terminating chunk. */
        if (c->is_writer && c->write_total == -1 && c->chunked) {
            (void)nha_send_all(c->fd, "0\r\n\r\n", 5);
        }
        nha_close_sock(c);
    }
    c->connected = false;
    return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    if (!c || c->fd < 0) return -1;
    /* Drain any already-written POST body that was queued before calling us. */
    if (c->is_writer && c->post_body && c->content_written < c->post_len) {
        int rem = c->post_len - c->content_written;
        int n = nha_send_all(c->fd, c->post_body + c->content_written, rem);
        if (n > 0) c->content_written += n;
    }
    int64_t cl = nha_parse_headers(c);
    return cl;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    return c ? c->status_code : 0;
}

esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c)
{
    /* gmf_io_http.c calls this after seeing a 301/302 status code to
     * request that the *next* esp_http_client_open() re-target to the
     * stored Location URL.  Since we already saved it while parsing
     * headers, we have nothing else to do here but confirm it's there. */
    return (c && c->redirect_target) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

int esp_http_client_get_errno(esp_http_client_handle_t c)
{
    return c ? c->last_errno : EINVAL;
}

int esp_http_client_get_socket(esp_http_client_handle_t c)
{
    return c ? c->fd : -1;
}

int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len)
{
    if (!c || !buf || len <= 0) return 0;
    if (c->fd < 0) return -1;
    int n = 0;
    if (c->write_total == -1) {
        /* Chunked: emit chunk size + \r\n, data, \r\n. */
        char head[16];
        int hl = snprintf(head, sizeof(head), "%x\r\n", len);
        if (nha_send_all(c->fd, head, hl) < 0) {
            c->last_errno = errno; return -1;
        }
        n = nha_send_all(c->fd, buf, len);
        if (n < 0) { c->last_errno = errno; return -1; }
        if (nha_send_all(c->fd, "\r\n", 2) < 0) {
            c->last_errno = errno; return -1;
        }
    } else {
        int to_write = len;
        if (c->write_total >= 0 && c->content_written + to_write > c->write_total) {
            to_write = c->write_total - c->content_written;
        }
        n = nha_send_all(c->fd, buf, to_write);
        if (n < 0) { c->last_errno = errno; return -1; }
        c->content_written += n;
    }
    return n;
}

/* Read body bytes, first draining any read-ahead bytes left in line_buf by
 * nha_read_line() during header parsing.  The response headers and the body
 * often arrive in the same TCP segment; without this drain the body bytes are
 * stranded in line_buf and esp_http_client_read() would hit EOF (return 0) for
 * small bodies such as .m3u8 playlists. */
static int nha_read_body(struct esp_http_client *c, char *buf, int want)
{
    int avail = c->line_len - c->line_pos;
    if (avail > 0) {
        if (want > avail) want = avail;
        memcpy(buf, c->line_buf + c->line_pos, want);
        c->line_pos += want;
        return want;
    }
    return nha_recv(c, buf, want);
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    if (!c || !buf || len <= 0) return -1;
    if (c->content_length >= 0 && c->body_remaining <= 0) {
        nha_dbg("NHA_READ cl_done\n");
        return 0;
    }
    if (c->fd < 0 && (c->line_len - c->line_pos) <= 0) return -1;

    if (c->body_read == 0 || (c->body_read % 64) == 0) {
        nha_dbg("NHA_READ call=%lld len=%d chunked=%d cl=%lld body_rem=%lld chunk_rem=%lld\n",
                (long long)c->body_read, len, c->chunked,
                (long long)c->content_length, (long long)c->body_remaining,
                (long long)c->chunk_remaining);
    }
    c->body_read++;

    if (c->chunked) {
        if (c->chunk_done) return 0;
        while (c->chunk_remaining <= 0) {
            int64_t sz = nha_read_chunk_size(c);
            if (sz < 0) { nha_dbg("NHA_READ chunk_sz_err=%d\n", errno); return -1; }
            if (sz == 0) { c->chunk_done = true; nha_dbg("NHA_READ chunk_done\n"); return 0; }
            c->chunk_remaining = sz;
        }
        int want = len;
        if ((int64_t)want > c->chunk_remaining) want = (int)c->chunk_remaining;
        int n = nha_read_body(c, buf, want);
        if (n < 0) {
            if (errno == EINTR) return 0;
            c->last_errno = errno; nha_dbg("NHA_READ chunk_err=%d\n", errno); return -1;
        }
        c->chunk_remaining -= n;
        if (c->chunk_remaining == 0) {
            /* Consume the trailing \r\n after chunk data. */
            char trailer[2]; int got = 0;
            while (got < 2) {
                int r = nha_read_body(c, trailer + got, 2 - got);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) break;
                got += r;
            }
        }
        return n;
    }

    /* Plain content-length read. */
    if (c->content_length >= 0) {
        if (c->body_remaining <= 0) { nha_dbg("NHA_READ cl_done\n"); return 0; }
        int want = len;
        if ((int64_t)want > c->body_remaining) want = (int)c->body_remaining;
        int n = nha_read_body(c, buf, want);
        if (n < 0) {
            if (errno == EINTR) return 0;
            c->last_errno = errno; nha_dbg("NHA_READ cl_err=%d\n", errno); return -1;
        }
        c->body_remaining -= n;
        if (n == 0 || c->body_remaining <= 0) {
            /* Connection: close. Drop the PCB so the next HLS playlist /
             * TS socket can recv — two concurrent TCP sockets starve WiFi RX. */
            nha_close_sock(c);
        }
        return n;
    }

    /* Unknown length: read until close. */
    int n = nha_read_body(c, buf, len);
    if (n < 0 && errno == EINTR) return 0;
    if (n < 0) { c->last_errno = errno; nha_dbg("NHA_READ unk_err=%d\n", errno); }
    return n;
}
