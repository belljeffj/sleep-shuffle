/* Minimal HTTP/1.0 client and JSON field lookup, enough for the Jellyfin calls the test pak makes.
 * Plain http:// only (home LAN or Tailscale). Video streams go through FFmpeg, not this. */
#define _GNU_SOURCE
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int parse_url(const char *url, char *host, size_t hn, char *port, size_t pn, const char **path) {
    if (strncasecmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(end - p));
    size_t hl = (size_t)((colon ? colon : end) - p);
    if (hl == 0 || hl >= hn) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    if (colon) {
        size_t pl = (size_t)(end - colon - 1);
        if (pl == 0 || pl >= pn) return -1;
        memcpy(port, colon + 1, pl);
        port[pl] = 0;
    } else {
        snprintf(port, pn, "80");
    }
    *path = slash ? slash : "/";
    return 0;
}

static int connect_timeout(const char *host, const char *port, int timeout_ms) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            r = poll(&pfd, 1, timeout_ms) == 1 ? 0 : -1;
            if (r == 0) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err) r = -1;
            }
        }
        if (r == 0) {
            fcntl(fd, F_SETFL, fl);
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_all(int fd, const char *buf, size_t n) {
    while (n) {
        ssize_t w = send(fd, buf, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Decodes a chunked body in place; returns the new length. */
static size_t dechunk(char *b, size_t n) {
    size_t in = 0, out = 0;
    while (in < n) {
        char *e;
        unsigned long sz = strtoul(b + in, &e, 16);
        char *nl = strstr(e, "\r\n");
        if (!nl || sz == 0) break;
        in = (size_t)(nl + 2 - b);
        if (in + sz > n) sz = n - in;
        memmove(b + out, b + in, sz);
        out += sz;
        in += sz + 2;
    }
    b[out] = 0;
    return out;
}

int http_request(const char *method, const char *url, const char *auth, const char *json_body, int timeout_ms,
                 HttpResp *out) {
    memset(out, 0, sizeof *out);
    char host[256], port[16];
    const char *path;
    if (parse_url(url, host, sizeof host, port, sizeof port, &path) != 0) return -1;
    int fd = connect_timeout(host, port, timeout_ms);
    if (fd < 0) return -2;

    size_t blen = json_body ? strlen(json_body) : 0;
    size_t cap = strlen(path) + strlen(host) + (auth ? strlen(auth) : 0) + blen + 512;
    char *req = malloc(cap);
    int n = snprintf(req, cap,
                     "%s %s HTTP/1.0\r\nHost: %s:%s\r\nAccept: application/json\r\nConnection: close\r\n"
                     "%s%s%s"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                     method, path, host, port, auth ? "Authorization: " : "", auth ? auth : "", auth ? "\r\n" : "",
                     blen);
    int ok = send_all(fd, req, (size_t)n) == 0 && (blen == 0 || send_all(fd, json_body, blen) == 0);
    free(req);
    if (!ok) {
        close(fd);
        return -3;
    }

    size_t len = 0, rcap = 16384;
    char *buf = malloc(rcap + 1);
    for (;;) {
        if (len == rcap) {
            if (rcap > (8u << 20)) break; /* 8 MB is far more than any reply we ask for */
            rcap *= 2;
            buf = realloc(buf, rcap + 1);
        }
        ssize_t r = recv(fd, buf + len, rcap - len, 0);
        if (r <= 0) break;
        len += (size_t)r;
    }
    close(fd);
    buf[len] = 0;

    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (len < 12 || !hdr_end) {
        free(buf);
        return -4;
    }
    out->status = atoi(buf + 9);
    *hdr_end = 0;
    int chunked = strcasestr(buf, "transfer-encoding: chunked") != NULL;
    char *body = hdr_end + 4;
    size_t bl = len - (size_t)(body - buf);
    memmove(buf, body, bl);
    buf[bl] = 0;
    if (chunked) bl = dechunk(buf, bl);
    out->body = buf;
    out->len = bl;
    return 0;
}

void http_free(HttpResp *r) {
    free(r->body);
    r->body = NULL;
}

static const char *find_key(const char *json, const char *key) {
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    return p;
}

int json_str(const char *json, const char *key, char *out, size_t n) {
    if (!json || n == 0) return -1;
    const char *p = find_key(json, key);
    if (!p || *p != '"') return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < n) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'n') c = ' ';
            else if (c == 't') c = ' ';
            else if (c == 'u') { /* keep it simple: show non-ASCII as '?' */
                c = '?';
                for (int i = 0; i < 4 && p[1]; i++) p++;
            }
            out[o++] = c;
            p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return 0;
}

int json_bool(const char *json, const char *key) {
    const char *p = json ? find_key(json, key) : NULL;
    if (!p) return -1;
    return strncmp(p, "true", 4) == 0 ? 1 : 0;
}

long long json_num(const char *json, const char *key, long long dflt) {
    const char *p = json ? find_key(json, key) : NULL;
    if (!p || (*p != '-' && (*p < '0' || *p > '9'))) return dflt;
    return strtoll(p, NULL, 10);
}
