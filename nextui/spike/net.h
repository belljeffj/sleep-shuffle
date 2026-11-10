#pragma once
#include <stddef.h>

typedef struct {
    int status;
    char *body;
    size_t len;
} HttpResp;

/* 0 on success (any HTTP status), negative on connection or parse failure. Caller frees with http_free. */
int http_request(const char *method, const char *url, const char *auth, const char *json_body, int timeout_ms,
                 HttpResp *out);
void http_free(HttpResp *r);

/* First occurrence of "key": in json. Jellyfin writes compact JSON, so this is enough here. */
int json_str(const char *json, const char *key, char *out, size_t n);
int json_bool(const char *json, const char *key); /* 1, 0, or -1 if missing */
long long json_num(const char *json, const char *key, long long dflt);
