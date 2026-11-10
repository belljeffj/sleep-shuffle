#define _GNU_SOURCE
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

Config cfg;
char g_data_dir[512];
char g_base[256];
static char server_version[32];

/* ================================================================ settings */

static void path_in_data(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", g_data_dir, name); }

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

void normalize_url(char *url, size_t n) {
    rstrip(url);
    size_t l = strlen(url);
    while (l && url[l - 1] == '/') url[--l] = 0;
    if (l && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "http://%.240s", url);
        snprintf(url, n, "%s", tmp);
    }
}

static void shows_decode(const char *v) { /* id|name;id|name;... */
    cfg.nshows = 0;
    const char *p = v;
    while (*p && cfg.nshows < MAX_SHOWS) {
        const char *bar = strchr(p, '|');
        if (!bar) break;
        const char *semi = strchr(bar, ';');
        if (!semi) semi = bar + strlen(bar);
        Show *s = &cfg.shows[cfg.nshows];
        snprintf(s->id, sizeof s->id, "%.*s", (int)(bar - p), p);
        snprintf(s->name, sizeof s->name, "%.*s", (int)(semi - bar - 1), bar + 1);
        if (s->id[0]) cfg.nshows++;
        p = *semi ? semi + 1 : semi;
    }
}

static void cfg_load(void) {
    char path[600], line[4096];
    path_in_data(path, sizeof path, "config.txt");
    cfg.window = 50;
    cfg.dim = 0;
    snprintf(cfg.last_timer, sizeof cfg.last_timer, "min:45");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        rstrip(v);
#define S(key, field) else if (!strcmp(line, key)) snprintf(cfg.field, sizeof cfg.field, "%s", v);
        if (0) {}
        S("lan", lan) S("server", lan) /* "server" is the test pak's name for it */
        S("tailscale", tailscale) S("token", token) S("user_id", user_id) S("user_name", user_name)
        S("device_id", device_id) S("last_selection", last_sel) S("last_timer", last_timer)
#undef S
        else if (!strcmp(line, "shows")) shows_decode(v);
        else if (!strcmp(line, "recent_window")) cfg.window = atoi(v);
        else if (!strcmp(line, "dim")) cfg.dim = atoi(v);
    }
    fclose(f);
    normalize_url(cfg.lan, sizeof cfg.lan);
    normalize_url(cfg.tailscale, sizeof cfg.tailscale);
}

void cfg_save(void) {
    char path[600], tmp[620];
    path_in_data(path, sizeof path, "config.txt");
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        logm("could not write %s", tmp);
        return;
    }
    fprintf(f, "# Sleep Shuffle settings. Edit with the app, or here while the app is closed.\n");
    fprintf(f, "lan=%s\ntailscale=%s\ntoken=%s\nuser_id=%s\nuser_name=%s\ndevice_id=%s\n", cfg.lan, cfg.tailscale,
            cfg.token, cfg.user_id, cfg.user_name, cfg.device_id);
    fprintf(f, "shows=");
    for (int i = 0; i < cfg.nshows; i++) fprintf(f, "%s|%s;", cfg.shows[i].id, cfg.shows[i].name);
    fprintf(f, "\nrecent_window=%d\nlast_selection=%s\nlast_timer=%s\ndim=%d\n", cfg.window, cfg.last_sel,
            cfg.last_timer, cfg.dim);
    fflush(f);
    fclose(f);
    rename(tmp, path);
}

/* ================================================================ history and bans */

#define HISTORY_MAX 600
static char (*history)[40];
static int nhistory;

static Ban *bans;
static int nbans, cap_bans;

static void history_load(void) {
    char path[600], line[128];
    path_in_data(path, sizeof path, "history.txt");
    history = calloc(HISTORY_MAX + 1, sizeof *history);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        rstrip(line);
        if (!line[0]) continue;
        if (nhistory == HISTORY_MAX) {
            memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof *history);
            nhistory--;
        }
        snprintf(history[nhistory++], sizeof history[0], "%s", line);
    }
    fclose(f);
}

static void history_save(void) {
    char path[600], tmp[620];
    path_in_data(path, sizeof path, "history.txt");
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < nhistory; i++) fprintf(f, "%s\n", history[i]);
    fclose(f);
    rename(tmp, path);
}

void history_add(const char *id) {
    if (nhistory == HISTORY_MAX) {
        memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof *history);
        nhistory--;
    }
    snprintf(history[nhistory++], sizeof history[0], "%s", id);
    history_save();
}
int history_count(void) { return nhistory; }
const char *history_at(int i) { return history[i]; }

static void bans_load(void) {
    char path[600], line[400];
    path_in_data(path, sizeof path, "banned.txt");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        rstrip(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        ban_add(line, tab + 1);
    }
    fclose(f);
}

static void bans_save(void) {
    char path[600], tmp[620];
    path_in_data(path, sizeof path, "banned.txt");
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < nbans; i++) fprintf(f, "%s\t%s\n", bans[i].id, bans[i].label);
    fclose(f);
    rename(tmp, path);
}

int bans_count(void) { return nbans; }
const Ban *ban_at(int i) { return &bans[i]; }
int is_banned(const char *id) {
    for (int i = 0; i < nbans; i++)
        if (!strcmp(bans[i].id, id)) return 1;
    return 0;
}
void ban_add(const char *id, const char *label) {
    if (is_banned(id)) return;
    if (nbans == cap_bans) {
        cap_bans = cap_bans ? cap_bans * 2 : 16;
        bans = realloc(bans, (size_t)cap_bans * sizeof *bans);
    }
    snprintf(bans[nbans].id, sizeof bans[0].id, "%s", id);
    snprintf(bans[nbans].label, sizeof bans[0].label, "%s", label);
    nbans++;
    bans_save();
}
void ban_remove(const char *id) {
    for (int i = 0; i < nbans; i++)
        if (!strcmp(bans[i].id, id)) {
            memmove(&bans[i], &bans[i + 1], (size_t)(nbans - i - 1) * sizeof *bans);
            nbans--;
            bans_save();
            return;
        }
}

void store_init(const char *data_dir) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s", data_dir);
    cfg_load();
    history_load();
    bans_load();
    if (!cfg.device_id[0]) {
        srand((unsigned)time(NULL));
        for (int i = 0; i < 24; i++) cfg.device_id[i] = "0123456789abcdef"[rand() % 16];
        cfg.device_id[24] = 0;
    }
    cfg_save();
}

/* ================================================================ Jellyfin */

void auth_header(char *out, size_t n) {
    snprintf(out, n, "MediaBrowser Client=\"Sleep Shuffle\", Device=\"Anbernic RG SP\", DeviceId=\"%s\", Version=\"%s\"%s%s%s",
             cfg.device_id, APP_VERSION, cfg.token[0] ? ", Token=\"" : "", cfg.token, cfg.token[0] ? "\"" : "");
}

static int req(const char *method, const char *base, const char *path, const char *body, int timeout_ms, HttpResp *r) {
    char url[1200], auth[512];
    snprintf(url, sizeof url, "%s%s", base, path);
    auth_header(auth, sizeof auth);
    int rc = http_request(method, url, auth, body, timeout_ms, r);
    /* query strings can carry ids but never the token, which only travels in the header here */
    logm("%s %.160s -> rc=%d status=%d", method, path, rc, r->status);
    return rc;
}

static int jget(const char *path, HttpResp *r) {
    if (!g_base[0] && jf_resolve(0) != 0) {
        memset(r, 0, sizeof *r);
        return -1;
    }
    int rc = req("GET", g_base, path, NULL, 8000, r);
    if (rc != 0) { /* address may have changed (left home Wi-Fi): pick again and retry once */
        http_free(r);
        if (jf_resolve(1) != 0) return -1;
        rc = req("GET", g_base, path, NULL, 8000, r);
    }
    return rc;
}

int jf_ping(const char *base) {
    if (!base[0]) return 0;
    if (strncmp(base, "https://", 8) == 0) {
        logm("ping %s: https is not supported yet", base);
        return 0;
    }
    HttpResp r;
    int ok = req("GET", base, "/System/Info/Public", NULL, 3000, &r) == 0 && r.status == 200;
    if (ok && r.body) json_str(r.body, "Version", server_version, sizeof server_version);
    http_free(&r);
    return ok;
}

int jf_resolve(int force) {
    if (g_base[0] && !force) return 0;
    if (jf_ping(cfg.lan)) snprintf(g_base, sizeof g_base, "%s", cfg.lan);
    else if (jf_ping(cfg.tailscale)) snprintf(g_base, sizeof g_base, "%s", cfg.tailscale);
    else {
        g_base[0] = 0;
        return -1;
    }
    logm("using server %s (Jellyfin %s)", g_base, server_version);
    return 0;
}

const char *jf_server_version(void) { return server_version; }

int jf_check_token(void) {
    if (!cfg.token[0] || !cfg.user_id[0]) return 0;
    if (jf_resolve(0) != 0) return -1;
    char path[128];
    HttpResp r;
    snprintf(path, sizeof path, "/Users/%s", cfg.user_id);
    int rc = jget(path, &r);
    int st = r.status;
    http_free(&r);
    if (rc != 0) return -1;
    return st == 200 ? 1 : (st == 401 || st == 403 || st == 404) ? 0 : -1;
}

int jf_qc_initiate(char *code, size_t cn, char *secret, size_t sn) {
    HttpResp r;
    if (jf_resolve(0) != 0) return -1;
    req("POST", g_base, "/QuickConnect/Initiate", NULL, 6000, &r);
    if (r.status == 404 || r.status == 405) { /* servers before 10.9 used GET */
        http_free(&r);
        req("GET", g_base, "/QuickConnect/Initiate", NULL, 6000, &r);
    }
    int st = r.status, ok = st == 200 && json_str(r.body, "Code", code, cn) == 0 &&
                             json_str(r.body, "Secret", secret, sn) == 0;
    http_free(&r);
    if (ok) return 0;
    return st == 401 ? 401 : -1;
}

int jf_qc_poll(const char *secret) {
    char path[300];
    HttpResp r;
    snprintf(path, sizeof path, "/QuickConnect/Connect?Secret=%s", secret);
    if (req("GET", g_base, path, NULL, 5000, &r) != 0 || r.status != 200) {
        http_free(&r);
        return -1;
    }
    int a = json_bool(r.body, "Authenticated");
    http_free(&r);
    return a == 1;
}

int jf_qc_finish(const char *secret) {
    char body[300];
    HttpResp r;
    snprintf(body, sizeof body, "{\"Secret\":\"%s\"}", secret);
    if (req("POST", g_base, "/Users/AuthenticateWithQuickConnect", body, 8000, &r) != 0 || r.status != 200 ||
        json_str(r.body, "AccessToken", cfg.token, sizeof cfg.token) != 0) {
        http_free(&r);
        return -1;
    }
    const char *u = strstr(r.body, "\"User\":");
    if (u) {
        json_str(u, "Id", cfg.user_id, sizeof cfg.user_id);
        json_str(u, "Name", cfg.user_name, sizeof cfg.user_name);
    }
    http_free(&r);
    cfg_save();
    logm("signed in as %s", cfg.user_name);
    return 0;
}

/* Copies one array object into a NUL-terminated buffer so the json_* lookups stay inside it. */
static char *obj_dup(const char *s, const char *e) {
    size_t n = (size_t)(e - s);
    char *b = malloc(n + 1);
    memcpy(b, s, n);
    b[n] = 0;
    return b;
}

int jf_series(Show *out, int max) {
    char path[300];
    HttpResp r;
    snprintf(path, sizeof path,
             "/Items?userId=%s&includeItemTypes=Series&recursive=true&sortBy=SortName&sortOrder=Ascending", cfg.user_id);
    if (jget(path, &r) != 0 || r.status != 200) {
        http_free(&r);
        return -1;
    }
    int n = 0;
    const char *p = strstr(r.body, "\"Items\":[");
    const char *s, *e;
    if (p) p += 9;
    while (p && n < max && (p = json_next_object(p, &s, &e))) {
        char *o = obj_dup(s, e);
        if (json_str(o, "Id", out[n].id, sizeof out[n].id) == 0) {
            json_str(o, "Name", out[n].name, sizeof out[n].name);
            n++;
        }
        free(o);
    }
    http_free(&r);
    return n;
}

int jf_episodes(const char *series_id, const char *series_name, Episode **out) {
    char path[300];
    HttpResp r;
    snprintf(path, sizeof path, "/Shows/%s/Episodes?userId=%s&isMissing=false", series_id, cfg.user_id);
    *out = NULL;
    if (jget(path, &r) != 0 || r.status != 200) {
        http_free(&r);
        return -1;
    }
    int n = 0, cap = 0;
    Episode *list = NULL;
    const char *p = strstr(r.body, "\"Items\":[");
    const char *s, *e;
    if (p) p += 9;
    while (p && (p = json_next_object(p, &s, &e))) {
        char *o = obj_dup(s, e);
        char loc[24] = "";
        json_str(o, "LocationType", loc, sizeof loc);
        int season = (int)json_num(o, "ParentIndexNumber", 0);
        if (season > 0 && strcmp(loc, "Virtual") != 0) { /* SPEC: no specials, no missing files */
            if (n == cap) {
                cap = cap ? cap * 2 : 128;
                list = realloc(list, (size_t)cap * sizeof *list);
            }
            Episode *ep = &list[n];
            memset(ep, 0, sizeof *ep);
            if (json_str(o, "Id", ep->id, sizeof ep->id) == 0) {
                snprintf(ep->series_id, sizeof ep->series_id, "%s", series_id);
                if (json_str(o, "SeriesName", ep->series, sizeof ep->series) != 0 || !ep->series[0])
                    snprintf(ep->series, sizeof ep->series, "%s", series_name);
                json_str(o, "Name", ep->name, sizeof ep->name);
                ep->season = season;
                ep->number = (int)json_num(o, "IndexNumber", 0);
                ep->runtime = json_num(o, "RunTimeTicks", 0) / 1e7;
                n++;
            }
        }
        free(o);
    }
    http_free(&r);
    *out = list;
    return n;
}

int jf_segments(const char *item_id, Segment *out, int max) {
    char path[300];
    HttpResp r;
    int n = 0;
    snprintf(path, sizeof path, "/MediaSegments/%s?includeSegmentTypes=Intro&includeSegmentTypes=Recap", item_id);
    if (jget(path, &r) == 0 && r.status == 200) {
        const char *p = strstr(r.body, "\"Items\":[");
        const char *s, *e;
        if (p) p += 9;
        while (p && n < max && (p = json_next_object(p, &s, &e))) {
            char *o = obj_dup(s, e);
            char type[16] = "";
            json_str(o, "Type", type, sizeof type);
            double a = json_num(o, "StartTicks", 0) / 1e7, b = json_num(o, "EndTicks", 0) / 1e7;
            if ((!strcmp(type, "Intro") || !strcmp(type, "Recap")) && b > a) {
                snprintf(out[n].type, sizeof out[n].type, "%s", type);
                out[n].start = a;
                out[n].end = b;
                n++;
            }
            free(o);
        }
    }
    http_free(&r);
    if (n) return n;
    /* older Intro Skipper plugin API */
    snprintf(path, sizeof path, "/Episode/%s/IntroTimestamps/v1", item_id);
    if (jget(path, &r) == 0 && r.status == 200 && json_bool(r.body, "Valid") == 1) {
        const char *a = strstr(r.body, "\"IntroStart\":"), *b = strstr(r.body, "\"IntroEnd\":");
        double s = a ? atof(a + 13) : 0, e = b ? atof(b + 11) : 0;
        if (e > s && max > 0) {
            snprintf(out[0].type, sizeof out[0].type, "Intro");
            out[0].start = s;
            out[0].end = e;
            n = 1;
        }
    }
    http_free(&r);
    return n;
}

void jf_stop_encoding(const char *play_session) {
    char path[300];
    HttpResp r;
    if (!g_base[0]) return;
    snprintf(path, sizeof path, "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s", cfg.device_id, play_session);
    if (req("DELETE", g_base, path, NULL, 3000, &r) == 0) http_free(&r);
}

/* ================================================================ shuffle */

/* Port of the Android Shuffler: skip banned and queued episodes, and the most recent `window`
 * distinct plays from this pool. The window shrinks to one less than the pickable count. */
const Episode *shuffle_pick(const Episode *pool, int n, const char *exclude_id) {
    if (n <= 0) return NULL;
    int *allowed = malloc((size_t)n * sizeof *allowed), na = 0;
    for (int i = 0; i < n; i++)
        if (!is_banned(pool[i].id) && !(exclude_id && !strcmp(pool[i].id, exclude_id))) allowed[na++] = i;
    const Episode *pick = NULL;
    if (na == 0) {
        for (int i = 0; i < n; i++)
            if (!is_banned(pool[i].id)) allowed[na++] = i;
        pick = na ? &pool[allowed[rand() % na]] : &pool[rand() % n];
        free(allowed);
        return pick;
    }
    int window = cfg.window;
    if (window > na - 1) window = na - 1;
    if (window < 0) window = 0;
    /* mark the most recent `window` distinct ids from history that are in the allowed set */
    char *recent = calloc((size_t)na, 1);
    int found = 0;
    for (int h = history_count() - 1; h >= 0 && found < window; h--) {
        const char *id = history_at(h);
        for (int k = 0; k < na; k++)
            if (!recent[k] && !strcmp(pool[allowed[k]].id, id)) {
                recent[k] = 1;
                found++;
                break;
            }
    }
    int nf = 0;
    for (int k = 0; k < na; k++)
        if (!recent[k]) allowed[nf++] = allowed[k];
    pick = nf ? &pool[allowed[rand() % nf]] : NULL;
    if (!pick) { /* cannot happen given the window cap, but stay safe */
        pick = &pool[rand() % n];
    }
    free(recent);
    free(allowed);
    return pick;
}
