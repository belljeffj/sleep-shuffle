/* Sleep Shuffle for NextUI: settings, history, bans, Jellyfin client and the shuffle rules.
 * Behavior follows SPEC.md at the repo root, the same rules as the Android app. */
#pragma once
#include <stddef.h>

#define MAX_SHOWS 64

typedef struct {
    char id[40];
    char name[128];
} Show;

typedef struct {
    char id[40];
    char series_id[40];
    char series[128];
    int season, number;
    char name[160];
    double runtime; /* seconds, 0 if unknown */
} Episode;

typedef struct {
    char type[8]; /* "Intro" or "Recap" */
    double start, end;
} Segment;

/* ---- settings (DATA/config.txt) */
typedef struct {
    char lan[256], tailscale[256];
    char token[128], user_id[64], user_name[128], device_id[64];
    Show shows[MAX_SHOWS];
    int nshows;
    int window;          /* skip the last N plays per show (0..300) */
    char last_sel[48];   /* show id or "ALL" */
    char last_timer[24]; /* TimerSpec encoding, as on Android: off, min:45, ep:2, clock:23:30 */
    int dim;             /* 0..3 extra darkening over video */
} Config;

extern Config cfg;
extern char g_data_dir[512];

void store_init(const char *data_dir);
void cfg_save(void);
void normalize_url(char *url, size_t n);

/* history: episode ids, oldest first, last 600 kept */
void history_add(const char *id);
int history_count(void);
const char *history_at(int i);

/* bans */
typedef struct {
    char id[40];
    char label[200];
} Ban;
int bans_count(void);
const Ban *ban_at(int i);
int is_banned(const char *id);
void ban_add(const char *id, const char *label);
void ban_remove(const char *id);

/* ---- Jellyfin */
extern char g_base[256]; /* the address in use: home first, Tailscale second */
void auth_header(char *out, size_t n);
int jf_ping(const char *base);
int jf_resolve(int force);  /* 0 ok, -1 neither address answers */
int jf_check_token(void);   /* 1 token works, 0 rejected, -1 unreachable */
int jf_qc_initiate(char *code, size_t cn, char *secret, size_t sn); /* 0 ok, 401 off, -1 error */
int jf_qc_poll(const char *secret);                                  /* 1 approved, 0 waiting, -1 error */
int jf_qc_finish(const char *secret);                                /* 0 signed in */
int jf_series(Show *out, int max);                                   /* count, -1 error */
int jf_episodes(const char *series_id, const char *series_name, Episode **out); /* count, -1 error */
int jf_segments(const char *item_id, Segment *out, int max);
void jf_stop_encoding(const char *play_session);
const char *jf_server_version(void);

/* ---- shuffle (SPEC.md "Shuffle") */
const Episode *shuffle_pick(const Episode *pool, int n, const char *exclude_id);

void logm(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
