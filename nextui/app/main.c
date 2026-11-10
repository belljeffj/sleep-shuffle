/* Sleep Shuffle for NextUI on the Anbernic RG SP (H700).
 * Shuffles episodes of chosen shows from Jellyfin for falling asleep. Rules: SPEC.md at the repo root.
 *
 * Args: <data dir> <log dir> <pak dir>. Exit code 42 asks launch.sh to install a staged update. */
#define _GNU_SOURCE
#include <SDL.h>
#include <execinfo.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core.h"
#include "player.h"
#include "ui.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define FADE_SECS 45.0
#define EXTEND_MIN 15
#define OVERLAY_MS 5000
#define AUDIO_BLANK_MS 5000
#define HOLD_MS 1200

/* ================================================================ logging and crash safety */

static FILE *g_log;

void logm(const char *fmt, ...) {
    if (!g_log) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(g_log, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void screen_power(int on);
static void screen_on_at_exit(void);

static void on_crash(int sig) {
    screen_power(1);
    if (g_log) {
        int fd = fileno(g_log);
        char head[64];
        int n = snprintf(head, sizeof head, "CRASH: signal %d\n", sig);
        if (write(fd, head, (size_t)n) < 0) {}
        void *frames[48];
        backtrace_symbols_fd(frames, backtrace(frames, 48), fd);
        fsync(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ================================================================ device bits */

static int g_blank;

/* NextUI blanks the screen through the framebuffer; the RG SP has no backlight power switch. */
static void screen_power(int on) {
    if (on == !g_blank) return;
    int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, on ? "0" : "4", 1) < 0) logm("screen blank write failed");
        close(fd);
    }
    g_blank = !on;
}

static void screen_on_at_exit(void) { screen_power(1); }

static int battery_pct(void) {
    glob_t g;
    int v = -1;
    if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &g) == 0 && g.gl_pathc) {
        FILE *f = fopen(g.gl_pathv[0], "r");
        if (f) {
            if (fscanf(f, "%d", &v) != 1) v = -1;
            fclose(f);
        }
    }
    globfree(&g);
    return v;
}

/* Power button and lid come from evdev, not SDL (NextUI reads them the same way). */
static int g_ev[8], g_nev;
static void evdev_open(void) {
    glob_t g;
    if (glob("/dev/input/event*", 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc && g_nev < 8; i++) {
        int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[80] = "";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        logm("input %s: %s", g.gl_pathv[i], name);
        g_ev[g_nev++] = fd;
    }
    globfree(&g);
}
/* 1 = power button pressed, 2 = lid closed, 3 = lid opened */
static int evdev_poll(void) {
    struct input_event e;
    for (int i = 0; i < g_nev; i++)
        while (read(g_ev[i], &e, sizeof e) == (ssize_t)sizeof e) {
            if (e.type == EV_KEY && e.code == KEY_POWER && e.value == 1) return 1;
            if (e.type == EV_SW && e.code == SW_LID) return e.value ? 2 : 3;
        }
    return 0;
}

/* Sleeps the device the way NextUI does, if its suspend helper is there. Blocks until wake. */
static void device_sleep(void) {
    const char *sys = getenv("SYSTEM_PATH");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "%s/bin/suspend", sys ? sys : "/mnt/SDCARD/.system/h700");
    struct stat st;
    screen_power(0);
    if (stat(cmd, &st) == 0) {
        logm("sleeping via %s", cmd);
        if (system(cmd) != 0) logm("suspend helper returned non-zero");
    } else {
        logm("no suspend helper at %s; screen off until a button is pressed", cmd);
        return; /* the caller keeps the screen off and waits for a button */
    }
    screen_power(1);
}

/* ================================================================ input */

enum { B_NONE, B_A, B_B, B_X, B_Y, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_START, B_SELECT, B_MENU, B_L, B_R };
static const char *bname[] = {"", "A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "START", "SELECT", "MENU", "L", "R"};

/* SDL names face buttons by position; the RG SP is printed SNES-style (A right, B bottom, X top, Y left). */
static int map_controller(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return B_B;
    case SDL_CONTROLLER_BUTTON_B: return B_A;
    case SDL_CONTROLLER_BUTTON_X: return B_Y;
    case SDL_CONTROLLER_BUTTON_Y: return B_X;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return B_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return B_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return B_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return B_RIGHT;
    case SDL_CONTROLLER_BUTTON_START: return B_START;
    case SDL_CONTROLLER_BUTTON_BACK: return B_SELECT;
    case SDL_CONTROLLER_BUTTON_GUIDE: return B_MENU;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return B_L;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return B_R;
    default: return B_NONE;
    }
}
static int map_key(SDL_Keycode k) { /* desktop testing */
    switch (k) {
    case SDLK_RETURN: case SDLK_z: return B_A;
    case SDLK_BACKSPACE: case SDLK_ESCAPE: case SDLK_x: return B_B;
    case SDLK_s: return B_X;
    case SDLK_a: return B_Y;
    case SDLK_UP: return B_UP;
    case SDLK_DOWN: return B_DOWN;
    case SDLK_LEFT: return B_LEFT;
    case SDLK_RIGHT: return B_RIGHT;
    case SDLK_SPACE: return B_START;
    case SDLK_TAB: return B_SELECT;
    case SDLK_m: return B_MENU;
    default: return B_NONE;
    }
}

static int a_held_since; /* ms when A went down, 0 if up */
static char shot_name[64]; /* desktop self-test: save the next frame as a BMP */

/* ================================================================ sleep timer (SPEC.md "Sleep timer") */

typedef struct {
    int kind; /* 0 off, 1 minutes, 2 episodes, 3 clock */
    int val, h, m;
} Timer;

static Timer timer_decode(const char *s) {
    Timer t = {1, 45, 23, 0};
    if (!strcmp(s, "off")) t.kind = 0;
    else if (!strncmp(s, "min:", 4)) t.kind = 1, t.val = atoi(s + 4);
    else if (!strncmp(s, "ep:", 3)) t.kind = 2, t.val = atoi(s + 3);
    else if (!strncmp(s, "clock:", 6)) {
        t.kind = 3;
        sscanf(s + 6, "%d:%d", &t.h, &t.m);
    }
    if ((t.kind == 1 || t.kind == 2) && t.val <= 0) t.val = t.kind == 1 ? 45 : 1;
    return t;
}
static void timer_encode(Timer t, char *out, size_t n) {
    if (t.kind == 0) snprintf(out, n, "off");
    else if (t.kind == 1) snprintf(out, n, "min:%d", t.val);
    else if (t.kind == 2) snprintf(out, n, "ep:%d", t.val);
    else snprintf(out, n, "clock:%d:%d", t.h, t.m);
}
static void clock_label(int h, int m, char *out, size_t n) {
    snprintf(out, n, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
}
static void timer_short(Timer t, char *out, size_t n) {
    char c[16];
    if (t.kind == 0) snprintf(out, n, "No timer");
    else if (t.kind == 1) snprintf(out, n, "%d min", t.val);
    else if (t.kind == 2) snprintf(out, n, t.val == 1 ? "1 episode" : "%d episodes", t.val);
    else {
        clock_label(t.h, t.m, c, sizeof c);
        snprintf(out, n, "%s", c);
    }
}

static const Timer PRESETS[] = {{0, 0, 0, 0},  {1, 15, 0, 0}, {1, 30, 0, 0}, {1, 45, 0, 0}, {1, 60, 0, 0},
                                {1, 90, 0, 0}, {1, 120, 0, 0}, {2, 1, 0, 0}, {2, 2, 0, 0}, {2, 3, 0, 0}};
#define NPRESETS (int)(sizeof PRESETS / sizeof *PRESETS)

static int timer_eq(Timer a, Timer b) {
    return a.kind == b.kind && (a.kind == 0 || (a.kind == 3 ? (a.h == b.h && a.m == b.m) : a.val == b.val));
}
static Timer timer_step(Timer t, int dir) { /* left/right through the presets */
    int i = 0;
    for (; i < NPRESETS; i++)
        if (timer_eq(PRESETS[i], t)) break;
    if (i == NPRESETS) i = 3;
    i = (i + dir + NPRESETS) % NPRESETS;
    return PRESETS[i];
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double next_clock(int h, int m) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = 0;
    time_t target = mktime(&tm);
    if (target <= t + 60) target += 24 * 3600;
    return (double)target;
}

/* ================================================================ app state */

enum { SCR_BOOT, SCR_KEYBOARD, SCR_QC, SCR_PICK, SCR_HOME, SCR_TIMER, SCR_PLAYER, SCR_SETTINGS, SCR_BANS, SCR_MESSAGE };

static int screen = SCR_BOOT, sel, running = 1, exit_code = 0;
static char status[200];
static char pak_dir[512];

/* message screen */
static char msg_title[120], msg_body[300];
static int msg_retry_screen; /* where A goes */

/* keyboard */
static char kb_buf[256], kb_title[80];
static char *kb_target;
static size_t kb_target_n;
static int kb_return, kb_row, kb_col;
static const char *KB_ROWS[] = {"1234567890", "qwertyuiop", "asdfghjkl:", "zxcvbnm./-"};

/* quick connect */
static char qc_code[16], qc_secret[160];
static Uint32 qc_next;

/* show picker */
static Show *library;
static int nlibrary, pick_return;

/* home */
static Timer home_timer;

/* playback */
typedef struct {
    char show_id[40];
    Episode *eps;
    int n;
} Pool;
static Pool pools[MAX_SHOWS];
static int npools;
static char play_sel[48];
static const Episode *cur;
static Episode cur_copy;
static Segment segs[4];
static int nsegs;
static Timer ptimer;
static double stop_at; /* wall seconds for minute and clock timers */
static int eps_left;
static float fade;
static int timer_done;
static Uint32 overlay_until, last_input, audio_since;
static int menu_open, menu_sel;
static SDL_Texture *vtex;
static int tw, th;
static int sleeping_screen_off;

/* settings */
static char update_tag[40];
static int update_state; /* 0 idle, 1 newer available, 2 up to date, 3 error */

static void go(int s) {
    screen = s;
    sel = 0;
    status[0] = 0;
}

static void message(const char *title, const char *body, int retry) {
    snprintf(msg_title, sizeof msg_title, "%s", title);
    snprintf(msg_body, sizeof msg_body, "%s", body);
    msg_retry_screen = retry;
    go(SCR_MESSAGE);
}

/* Draws a one-off frame so a blocking network call doesn't look frozen. */
static void busy(const char *what) {
    ui_clear();
    ui_text_center(FS_MED, SH / 2 - ui_text_h(FS_MED) / 2, GREY, what);
    SDL_RenderPresent(R);
}

static void keyboard(const char *title, char *target, size_t n, int ret) {
    snprintf(kb_title, sizeof kb_title, "%s", title);
    snprintf(kb_buf, sizeof kb_buf, "%s", target[0] ? target : "http://");
    kb_target = target;
    kb_target_n = n;
    kb_return = ret;
    kb_row = 1;
    kb_col = 0;
    go(SCR_KEYBOARD);
}

/* ================================================================ flow */

static void start_qc(void) {
    busy("Starting Quick Connect...");
    int rc = jf_qc_initiate(qc_code, sizeof qc_code, qc_secret, sizeof qc_secret);
    if (rc == 0) {
        qc_next = SDL_GetTicks() + 3000;
        go(SCR_QC);
    } else if (rc == 401)
        message("Quick Connect is off", "Turn it on in Jellyfin: Dashboard > General > Enable Quick Connect.", SCR_BOOT);
    else
        message("Couldn't start sign-in", "Jellyfin didn't answer. Check the address and the Wi-Fi.", SCR_BOOT);
}

static void open_picker(int ret) {
    busy("Loading your TV shows...");
    free(library);
    library = calloc(400, sizeof *library);
    nlibrary = jf_series(library, 400);
    pick_return = ret;
    if (nlibrary < 0) {
        message("Couldn't load your shows", "Jellyfin didn't answer. Check the Wi-Fi.", SCR_HOME);
        return;
    }
    go(SCR_PICK);
}

/* Decides where to start: setup steps first, then Home. */
static void boot(void) {
    if (!cfg.lan[0] && !cfg.tailscale[0]) {
        keyboard("Jellyfin home address", cfg.lan, sizeof cfg.lan, SCR_BOOT);
        return;
    }
    busy("Connecting to Jellyfin...");
    if (jf_resolve(1) != 0) {
        message("Can't reach Jellyfin", "Check the Wi-Fi, or press X to change the address.", SCR_BOOT);
        return;
    }
    int t = jf_check_token();
    if (t == 0) {
        start_qc();
        return;
    }
    if (t < 0) {
        message("Can't reach Jellyfin", "Check the Wi-Fi, or press X to change the address.", SCR_BOOT);
        return;
    }
    if (cfg.nshows == 0) {
        open_picker(SCR_HOME);
        return;
    }
    go(SCR_HOME);
}

static Pool *pool_for(const char *show_id, const char *name) {
    for (int i = 0; i < npools; i++)
        if (!strcmp(pools[i].show_id, show_id)) return &pools[i];
    if (npools == MAX_SHOWS) return NULL;
    Pool *p = &pools[npools];
    char what[200];
    snprintf(what, sizeof what, "Loading %s...", name);
    busy(what);
    int n = jf_episodes(show_id, name, &p->eps);
    if (n < 0) return NULL;
    snprintf(p->show_id, sizeof p->show_id, "%s", show_id);
    p->n = n;
    npools++;
    logm("%s: %d episodes", name, n);
    return p;
}

/* SPEC: the mix picks a show evenly first, then an episode from it. */
static const Episode *pick_next(void) {
    Pool *cand[MAX_SHOWS];
    int nc = 0;
    for (int i = 0; i < cfg.nshows; i++) {
        if (strcmp(play_sel, "ALL") != 0 && strcmp(play_sel, cfg.shows[i].id) != 0) continue;
        Pool *p = pool_for(cfg.shows[i].id, cfg.shows[i].name);
        if (p && p->n > 0) cand[nc++] = p;
    }
    if (!nc) return NULL;
    Pool *p = cand[rand() % nc];
    return shuffle_pick(p->eps, p->n, cur ? cur->id : NULL);
}

static void begin_episode(const Episode *ep, int audio_only) {
    cur_copy = *ep;
    cur = &cur_copy;
    logm("playing %s S%02dE%02d %s (%s)", ep->series, ep->season, ep->number, ep->name, ep->id);
    history_add(ep->id);
    nsegs = jf_segments(ep->id, segs, 4);
    player_play(ep->id, 0, audio_only, SW, SH);
    audio_since = SDL_GetTicks();
}

static void timer_arm(Timer t) {
    ptimer = t;
    timer_done = 0;
    fade = 0;
    player_set_gain(1);
    if (t.kind == 1) stop_at = now_s() + (getenv("SS_TIMER_SECS") ? atof(getenv("SS_TIMER_SECS")) : t.val * 60.0);
    else if (t.kind == 3) stop_at = next_clock(t.h, t.m);
    else stop_at = 0;
    eps_left = t.kind == 2 ? t.val : 0;
}

static void start_playback(const char *sel_key, Timer t, int remember) {
    snprintf(play_sel, sizeof play_sel, "%s", sel_key);
    if (remember) {
        snprintf(cfg.last_sel, sizeof cfg.last_sel, "%s", sel_key);
        timer_encode(t, cfg.last_timer, sizeof cfg.last_timer);
        cfg_save();
    }
    cur = NULL;
    const Episode *ep = pick_next();
    if (!ep) {
        message("No episodes", "Jellyfin has no episodes for that choice, or it didn't answer.", SCR_HOME);
        return;
    }
    timer_arm(t);
    menu_open = 0;
    overlay_until = SDL_GetTicks() + OVERLAY_MS;
    go(SCR_PLAYER);
    begin_episode(ep, 0);
}

static void stop_to_home(void) {
    player_stop();
    screen_power(1);
    cur = NULL;
    go(SCR_HOME);
}

static double remaining_s(void) { /* until the sleep timer stops playback; negative if no timer */
    if (ptimer.kind == 1 || ptimer.kind == 3) return stop_at - now_s();
    if (ptimer.kind == 2 && eps_left <= 1 && cur && cur->runtime > 0) return cur->runtime - player_position();
    return -1;
}

static const Segment *current_segment(void) {
    double pos = player_position();
    for (int i = 0; i < nsegs; i++)
        if (pos >= segs[i].start && pos < segs[i].end - 1.0) return &segs[i];
    return NULL;
}

static void extend_timer(void) {
    Timer t = {1, EXTEND_MIN, 0, 0};
    logm("+15 min");
    if (timer_done) { /* the timer had already stopped: carry on with a fresh episode */
        timer_done = 0;
        screen_power(1);
        const Episode *ep = pick_next();
        timer_arm(t);
        if (ep) begin_episode(ep, player_audio_only());
    } else
        timer_arm(t);
    overlay_until = SDL_GetTicks() + OVERLAY_MS;
}

/* ================================================================ updates (update.sh in the pak) */

static int run_update(const char *args, char *out, size_t n) {
    char cmd[800];
    snprintf(cmd, sizeof cmd, "sh \"%s/update.sh\" %s \"%s\" 2>>\"%s/update.log\"", pak_dir, args, g_data_dir, g_data_dir);
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (!fgets(out, (int)n, p)) out[0] = 0;
    int rc = pclose(p);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    logm("update.sh %s -> %d '%s'", args, rc, out);
    return rc;
}

static int version_newer(const char *a, const char *b) { /* a > b, dotted numbers */
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x > y;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
        if ((*a && *a != '.' && (*a < '0' || *a > '9')) || (*b && *b != '.' && (*b < '0' || *b > '9'))) break;
    }
    return 0;
}

static void check_updates(void) {
    char out[200];
    busy("Checking for updates...");
    if (run_update("check", out, sizeof out) == 0 && !strncmp(out, "latest nextui-v", 15)) {
        snprintf(update_tag, sizeof update_tag, "%s", out + 7);
        update_state = version_newer(update_tag + 8, APP_VERSION) ? 1 : 2;
    } else
        update_state = 3;
    snprintf(status, sizeof status, "%s",
             update_state == 1 ? "A newer version is ready. Press A to install it."
             : update_state == 2 ? "You have the latest version."
                                 : "Couldn't reach GitHub. Check the Wi-Fi.");
}

static void install_update(void) {
    char out[200], args[80];
    busy("Downloading the update...");
    snprintf(args, sizeof args, "download %s", update_tag);
    if (run_update(args, out, sizeof out) == 0 && !strcmp(out, "ready")) {
        busy("Installing. Sleep Shuffle will restart.");
        SDL_Delay(800);
        exit_code = 42;
        running = 0;
    } else
        snprintf(status, sizeof status, "The download didn't finish. Try again on Wi-Fi.");
}

/* ================================================================ screens: input */

static void kb_press(int b) {
    int rows = 4, keys_last = 3; /* bottom row: Delete, Clear, Done */
    size_t len = strlen(kb_buf);
    if (b == B_UP) kb_row = (kb_row + rows) % (rows + 1);
    else if (b == B_DOWN) kb_row = (kb_row + 1) % (rows + 1);
    else if (b == B_LEFT) kb_col = kb_row == rows ? (kb_col + keys_last - 1) % keys_last : (kb_col + 9) % 10;
    else if (b == B_RIGHT) kb_col = kb_row == rows ? (kb_col + 1) % keys_last : (kb_col + 1) % 10;
    if (kb_row == rows && kb_col >= keys_last) kb_col = keys_last - 1;
    if (b == B_B && len) kb_buf[len - 1] = 0;
    if (b == B_A) {
        if (kb_row < rows) {
            if (len + 1 < sizeof kb_buf) kb_buf[len] = KB_ROWS[kb_row][kb_col], kb_buf[len + 1] = 0;
        } else if (kb_col == 0 && len) kb_buf[len - 1] = 0;
        else if (kb_col == 1) kb_buf[0] = 0;
        else b = B_START;
    }
    if (b == B_START) {
        snprintf(kb_target, kb_target_n, "%s", kb_buf);
        normalize_url(kb_target, kb_target_n);
        if (!strcmp(kb_target, "http:") || !strcmp(kb_target, "http://")) kb_target[0] = 0;
        cfg_save();
        g_base[0] = 0;
        if (kb_return == SCR_BOOT) boot();
        else go(kb_return);
    }
    if (b == B_MENU || b == B_SELECT) { /* cancel */
        if (kb_return == SCR_BOOT && !cfg.lan[0]) running = 0;
        else go(kb_return == SCR_BOOT ? SCR_SETTINGS : kb_return);
    }
}

static int home_items(const char **labels, const char **values, int *kinds, char store[][128]);

enum { HK_LAST, HK_SHOW, HK_MIX, HK_TIMER, HK_SETTINGS };

static void home_press(int b) {
    const char *labels[MAX_SHOWS + 4], *values[MAX_SHOWS + 4];
    int kinds[MAX_SHOWS + 4];
    char store[MAX_SHOWS + 4][128];
    int n = home_items(labels, values, kinds, store);
    if (b == B_UP) sel = (sel + n - 1) % n;
    if (b == B_DOWN) sel = (sel + 1) % n;
    int k = kinds[sel];
    if (k == HK_TIMER && (b == B_LEFT || b == B_RIGHT)) {
        home_timer = timer_step(home_timer, b == B_LEFT ? -1 : 1);
        timer_encode(home_timer, cfg.last_timer, sizeof cfg.last_timer);
        cfg_save();
    }
    if (b == B_B) running = 0;
    if (b != B_A) return;
    if (k == HK_LAST) start_playback(cfg.last_sel, timer_decode(cfg.last_timer), 0);
    else if (k == HK_SHOW) {
        int first_show = kinds[0] == HK_LAST ? 1 : 0;
        start_playback(cfg.shows[sel - first_show].id, home_timer, 1);
    } else if (k == HK_MIX) start_playback("ALL", home_timer, 1);
    else if (k == HK_TIMER) go(SCR_TIMER);
    else if (k == HK_SETTINGS) {
        update_state = 0;
        go(SCR_SETTINGS);
    }
}

static Timer clock_choice = {3, 0, 23, 0};

static void timer_press(int b) {
    int n = NPRESETS + 1;
    if (b == B_UP) sel = (sel + n - 1) % n;
    if (b == B_DOWN) sel = (sel + 1) % n;
    if (sel == NPRESETS && (b == B_LEFT || b == B_RIGHT)) {
        int mins = clock_choice.h * 60 + clock_choice.m + (b == B_LEFT ? -15 : 15);
        mins = (mins + 24 * 60) % (24 * 60);
        clock_choice.h = mins / 60;
        clock_choice.m = mins % 60;
    }
    if (b == B_A) {
        home_timer = sel == NPRESETS ? clock_choice : PRESETS[sel];
        timer_encode(home_timer, cfg.last_timer, sizeof cfg.last_timer);
        cfg_save();
        go(SCR_HOME);
    }
    if (b == B_B) go(SCR_HOME);
}

enum { MI_TIMER, MI_SKIP, MI_BAN, MI_DIM, MI_HOME, MI_COUNT };

static void player_menu_press(int b) {
    if (b == B_UP) menu_sel = (menu_sel + MI_COUNT - 1) % MI_COUNT;
    if (b == B_DOWN) menu_sel = (menu_sel + 1) % MI_COUNT;
    if (menu_sel == MI_TIMER && (b == B_LEFT || b == B_RIGHT)) timer_arm(timer_step(ptimer, b == B_LEFT ? -1 : 1));
    if (menu_sel == MI_DIM && (b == B_LEFT || b == B_RIGHT)) {
        cfg.dim = (cfg.dim + (b == B_LEFT ? 3 : 1)) % 4;
        cfg_save();
    }
    if (b == B_A && menu_sel == MI_SKIP) {
        const Segment *s = current_segment();
        if (s && cur) player_play(cur->id, s->end, player_audio_only(), SW, SH);
        menu_open = 0;
    }
    if (b == B_A && menu_sel == MI_HOME) {
        menu_open = 0;
        stop_to_home();
    }
    if (b == B_B || b == B_X || b == B_MENU) menu_open = 0;
}

static void player_press(int b) {
    Uint32 now = SDL_GetTicks();
    last_input = now;
    if (g_blank) { /* any button wakes the screen and shows the controls */
        screen_power(1);
        overlay_until = now + OVERLAY_MS;
        audio_since = now;
        return;
    }
    if (menu_open) {
        player_menu_press(b);
        overlay_until = now + OVERLAY_MS;
        return;
    }
    if (timer_done) {
        if (b == B_A) extend_timer();
        else if (b == B_B) stop_to_home();
        return;
    }
    int fading = fade > 0;
    int overlay = now < overlay_until || fading || player_state() == PL_ERROR;
    const Segment *seg = current_segment();
    if (!overlay) { /* SPEC: the first press only brings up the controls */
        if (seg && b == B_X) {
            player_play(cur->id, seg->end, player_audio_only(), SW, SH);
            return;
        }
        overlay_until = now + OVERLAY_MS;
        return;
    }
    overlay_until = now + OVERLAY_MS;
    if (fading && b == B_A) {
        extend_timer();
        return;
    }
    if (player_state() == PL_ERROR && b == B_A && cur) {
        player_play(cur->id, player_position(), player_audio_only(), SW, SH);
        return;
    }
    switch (b) {
    case B_A:
        if (cur && (player_state() == PL_PLAYING || player_state() == PL_OPENING)) {
            int to_audio = !player_audio_only();
            double pos = player_position();
            logm("switching to %s at %.1fs", to_audio ? "audio only" : "video", pos);
            player_play(cur->id, pos, to_audio, SW, SH);
            audio_since = now;
        }
        break;
    case B_Y: {
        const Episode *ep = pick_next();
        if (ep) {
            if (ptimer.kind == 2 && eps_left > 1) eps_left--; /* skipping counts as finishing one */
            begin_episode(ep, player_audio_only());
        }
        break;
    }
    case B_X:
    case B_MENU:
        menu_open = 1;
        menu_sel = seg ? MI_SKIP : MI_TIMER;
        break;
    case B_B: stop_to_home(); break;
    default: break;
    }
}

enum { SI_SHOWS, SI_WINDOW, SI_BANS, SI_LAN, SI_TS, SI_TEST, SI_DIM, SI_UPDATE, SI_SIGNOUT, SI_COUNT };

static void settings_press(int b) {
    if (b == B_UP) sel = (sel + SI_COUNT - 1) % SI_COUNT;
    if (b == B_DOWN) sel = (sel + 1) % SI_COUNT;
    if (sel != SI_UPDATE && (b == B_UP || b == B_DOWN)) status[0] = 0;
    if (sel == SI_WINDOW && (b == B_LEFT || b == B_RIGHT)) {
        cfg.window += b == B_LEFT ? -10 : 10;
        if (cfg.window < 0) cfg.window = 0;
        if (cfg.window > 300) cfg.window = 300;
        cfg_save();
    }
    if (sel == SI_DIM && (b == B_LEFT || b == B_RIGHT)) {
        cfg.dim = (cfg.dim + (b == B_LEFT ? 3 : 1)) % 4;
        cfg_save();
    }
    if (b == B_B) go(SCR_HOME);
    if (b != B_A) return;
    switch (sel) {
    case SI_SHOWS: open_picker(SCR_SETTINGS); break;
    case SI_BANS: go(SCR_BANS); break;
    case SI_LAN: keyboard("Jellyfin home address", cfg.lan, sizeof cfg.lan, SCR_SETTINGS); break;
    case SI_TS: keyboard("Tailscale address (http only for now)", cfg.tailscale, sizeof cfg.tailscale, SCR_SETTINGS); break;
    case SI_TEST: {
        busy("Testing...");
        int h = jf_ping(cfg.lan), t = jf_ping(cfg.tailscale);
        snprintf(status, sizeof status, "Home: %s. Tailscale: %s.", h ? "reachable" : cfg.lan[0] ? "no answer" : "not set",
                 t ? "reachable" : cfg.tailscale[0] ? "no answer" : "not set");
        jf_resolve(1);
        sel = SI_TEST;
        break;
    }
    case SI_UPDATE:
        if (update_state == 1) install_update();
        else check_updates();
        sel = SI_UPDATE;
        break;
    default: break;
    }
}

static void settings_hold(void) {
    if (screen == SCR_SETTINGS && sel == SI_SIGNOUT) {
        logm("signed out");
        cfg.token[0] = cfg.user_id[0] = cfg.user_name[0] = 0;
        cfg_save();
        boot();
    } else if (screen == SCR_PLAYER && menu_open && menu_sel == MI_BAN && cur) {
        char label[200];
        snprintf(label, sizeof label, "%s S%02dE%02d %s", cur->series, cur->season, cur->number, cur->name);
        ban_add(cur->id, label);
        logm("banned %s", label);
        menu_open = 0;
        const Episode *ep = pick_next();
        if (ep) begin_episode(ep, player_audio_only());
    }
}

static void pick_press(int b) {
    if (nlibrary == 0) {
        if (b == B_B || b == B_A) go(pick_return);
        return;
    }
    if (b == B_UP) sel = (sel + nlibrary - 1) % nlibrary;
    if (b == B_DOWN) sel = (sel + 1) % nlibrary;
    if (b == B_LEFT) sel = sel > 6 ? sel - 6 : 0;
    if (b == B_RIGHT) sel = sel + 6 < nlibrary ? sel + 6 : nlibrary - 1;
    if (b == B_A) {
        Show *s = &library[sel];
        int at = -1;
        for (int i = 0; i < cfg.nshows; i++)
            if (!strcmp(cfg.shows[i].id, s->id)) at = i;
        if (at >= 0) {
            memmove(&cfg.shows[at], &cfg.shows[at + 1], (size_t)(cfg.nshows - at - 1) * sizeof(Show));
            cfg.nshows--;
        } else if (cfg.nshows < MAX_SHOWS) {
            /* keep the library's alphabetical order */
            cfg.shows[cfg.nshows++] = *s;
            for (int i = cfg.nshows - 1; i > 0; i--) {
                int a = -1, c = -1;
                for (int k = 0; k < nlibrary; k++) {
                    if (!strcmp(library[k].id, cfg.shows[i - 1].id)) a = k;
                    if (!strcmp(library[k].id, cfg.shows[i].id)) c = k;
                }
                if (a > c) {
                    Show t = cfg.shows[i];
                    cfg.shows[i] = cfg.shows[i - 1];
                    cfg.shows[i - 1] = t;
                }
            }
        }
        cfg_save();
        npools = 0; /* re-fetch episodes for the new choice */
    }
    if (b == B_B || b == B_START) {
        if (cfg.nshows == 0) {
            snprintf(status, sizeof status, "Pick at least one show.");
            return;
        }
        int keep = sel;
        go(pick_return);
        if (pick_return == SCR_SETTINGS) sel = SI_SHOWS;
        (void)keep;
    }
}

static void bans_press(int b) {
    int n = bans_count();
    if (b == B_B || n == 0) {
        if (b == B_B || b == B_A) {
            go(SCR_SETTINGS);
            sel = SI_BANS;
        }
        return;
    }
    if (b == B_UP) sel = (sel + n - 1) % n;
    if (b == B_DOWN) sel = (sel + 1) % n;
    if (b == B_A) {
        ban_remove(ban_at(sel)->id);
        if (sel >= bans_count() && sel > 0) sel--;
    }
}

static void press(int b) {
    logm("button %s (screen %d)", bname[b], screen);
    switch (screen) {
    case SCR_KEYBOARD: kb_press(b); break;
    case SCR_QC:
        if (b == B_B) {
            if (cfg.token[0]) go(SCR_SETTINGS);
            else keyboard("Jellyfin home address", cfg.lan, sizeof cfg.lan, SCR_BOOT);
        }
        break;
    case SCR_PICK: pick_press(b); break;
    case SCR_HOME: home_press(b); break;
    case SCR_TIMER: timer_press(b); break;
    case SCR_PLAYER: player_press(b); break;
    case SCR_SETTINGS: settings_press(b); break;
    case SCR_BANS: bans_press(b); break;
    case SCR_MESSAGE:
        if (b == B_A) {
            if (msg_retry_screen == SCR_BOOT) boot();
            else go(msg_retry_screen);
        } else if (b == B_X) {
            go(SCR_SETTINGS);
            sel = SI_LAN;
        } else if (b == B_B) {
            if (msg_retry_screen == SCR_BOOT && !cfg.token[0]) running = 0;
            else if (msg_retry_screen == SCR_BOOT) running = 0;
            else go(SCR_HOME);
        }
        break;
    default: break;
    }
}

/* ================================================================ screens: drawing */

static int home_items(const char **labels, const char **values, int *kinds, char store[][128]) {
    int n = 0;
    int last_ok = 0;
    if (cfg.last_sel[0]) {
        if (!strcmp(cfg.last_sel, "ALL")) last_ok = cfg.nshows > 1;
        else
            for (int i = 0; i < cfg.nshows; i++)
                if (!strcmp(cfg.shows[i].id, cfg.last_sel)) last_ok = 1;
    }
    if (last_ok) {
        char t[40], what[128];
        timer_short(timer_decode(cfg.last_timer), t, sizeof t);
        snprintf(what, sizeof what, "%s", "Mix");
        for (int i = 0; i < cfg.nshows; i++)
            if (!strcmp(cfg.shows[i].id, cfg.last_sel)) snprintf(what, sizeof what, "%s", cfg.shows[i].name);
        snprintf(store[n], 128, "%s, %s", what, t);
        labels[n] = "Same as last night";
        values[n] = store[n];
        kinds[n++] = HK_LAST;
    }
    for (int i = 0; i < cfg.nshows; i++) {
        labels[n] = cfg.shows[i].name;
        values[n] = NULL;
        kinds[n++] = HK_SHOW;
    }
    if (cfg.nshows > 1) {
        snprintf(store[n], 128, cfg.nshows == 2 ? "Mix both" : "Mix all %d", cfg.nshows);
        labels[n] = store[n];
        values[n] = NULL;
        kinds[n++] = HK_MIX;
    }
    timer_short(home_timer, store[n], 128);
    labels[n] = "Sleep timer";
    values[n] = store[n];
    kinds[n++] = HK_TIMER;
    labels[n] = "Settings";
    values[n] = NULL;
    kinds[n++] = HK_SETTINGS;
    return n;
}

static void draw_keyboard(void) {
    ui_title(kb_title);
    int y = PAD + ui_text_h(FS_MED) + PAD;
    ui_round_rect(PAD, y, SW - 2 * PAD, PILL_H, 8, (SDL_Color){30, 30, 30, 255});
    int tw_ = ui_text_w(FS_MED, kb_buf), maxw = SW - 4 * PAD;
    /* show the end of long text */
    const char *shown = kb_buf;
    while (tw_ > maxw && *shown) tw_ = ui_text_w(FS_MED, ++shown);
    int x = 2 * PAD + ui_text(FS_MED, 2 * PAD, y + (PILL_H - ui_text_h(FS_MED)) / 2, WHITE, shown, 0);
    SDL_Rect caret = {x + 2, y + PILL_H / 4, 3, PILL_H / 2};
    SDL_SetRenderDrawColor(R, 255, 255, 255, 255);
    SDL_RenderFillRect(R, &caret);
    int ky = y + PILL_H + PAD, kw = (SW - 2 * PAD) / 10, kh = (SH - ky - PILL_H - 2 * PAD) / 5;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++) {
            int on = kb_row == r && kb_col == c;
            int kx = PAD + c * kw, yy = ky + r * kh;
            ui_round_rect(kx + 2, yy + 2, kw - 4, kh - 4, 8, on ? WHITE : (SDL_Color){35, 35, 35, 255});
            char ch[2] = {KB_ROWS[r][c], 0};
            ui_text(FS_MED, kx + (kw - ui_text_w(FS_MED, ch)) / 2, yy + (kh - ui_text_h(FS_MED)) / 2, on ? BLACK : WHITE, ch, 0);
        }
    const char *bottom[] = {"Delete", "Clear", "Done"};
    int bw = (SW - 2 * PAD) / 3;
    for (int i = 0; i < 3; i++) {
        int on = kb_row == 4 && kb_col == i, yy = ky + 4 * kh;
        ui_round_rect(PAD + i * bw + 2, yy + 2, bw - 4, kh - 4, 8, on ? WHITE : (SDL_Color){35, 35, 35, 255});
        ui_text(FS_MED, PAD + i * bw + (bw - ui_text_w(FS_MED, bottom[i])) / 2, yy + (kh - ui_text_h(FS_MED)) / 2,
                on ? BLACK : WHITE, bottom[i], 0);
    }
    const char *h[] = {"B", "DELETE", "A", "TYPE", "START", "DONE"};
    const char *l[] = {"MENU", "CANCEL"};
    ui_hints(h, 3, l, 1);
}

static void draw_message(void) {
    ui_text_center(FS_MED, SH / 2 - ui_text_h(FS_MED) - PAD / 2, WHITE, msg_title);
    ui_text_center(FS_SMALL, SH / 2 + PAD / 2, GREY, msg_body);
    if (msg_retry_screen == SCR_BOOT) {
        const char *h[] = {"B", "QUIT", "A", "TRY AGAIN"};
        const char *l[] = {"X", "ADDRESS"};
        ui_hints(h, 2, l, 1);
    } else {
        const char *h[] = {"B", "BACK", "A", "OK"};
        ui_hints(h, 2, NULL, 0);
    }
}

static void draw_qc(void) {
    ui_title("Sign in with Quick Connect");
    ui_text_center(FS_BIG, SH / 2 - ui_text_h(FS_BIG), WHITE, qc_code);
    ui_text_center(FS_SMALL, SH / 2 + PAD, GREY, "On your phone, open Jellyfin > Settings >");
    ui_text_center(FS_SMALL, SH / 2 + PAD + ui_text_h(FS_SMALL), GREY, "Quick Connect, and enter this code.");
    const char *h[] = {"B", "BACK"};
    ui_hints(h, 1, NULL, 0);
}

static void draw_pick(void) {
    ui_title("Shows to shuffle");
    int top = PAD * 2 + ui_text_h(FS_MED), bottom = SH - PILL_H - 2 * PAD;
    if (nlibrary == 0) {
        ui_text_center(FS_MED, SH / 2, GREY, "Jellyfin has no TV shows.");
    } else {
        const char **labels = calloc((size_t)nlibrary, sizeof *labels), **values = calloc((size_t)nlibrary, sizeof *values);
        for (int i = 0; i < nlibrary; i++) {
            labels[i] = library[i].name;
            values[i] = "";
            for (int k = 0; k < cfg.nshows; k++)
                if (!strcmp(cfg.shows[k].id, library[i].id)) values[i] = "On";
        }
        ui_list(top, bottom, labels, values, nlibrary, sel);
        free(labels);
        free(values);
    }
    ui_status_line(status);
    const char *h[] = {"B", "DONE", "A", "ON / OFF"};
    ui_hints(h, 2, NULL, 0);
}

static void draw_home(void) {
    const char *labels[MAX_SHOWS + 4], *values[MAX_SHOWS + 4];
    int kinds[MAX_SHOWS + 4];
    char store[MAX_SHOWS + 4][128];
    int n = home_items(labels, values, kinds, store);
    if (sel >= n) sel = n - 1;
    ui_title("Sleep Shuffle");
    ui_list(PAD * 2 + ui_text_h(FS_MED), SH - PILL_H - 2 * PAD, labels, values, n, sel);
    if (kinds[sel] == HK_TIMER) {
        const char *h[] = {"B", "QUIT", "A", "MORE"};
        const char *l[] = {"<>", "CHANGE"};
        ui_hints(h, 2, l, 1);
    } else {
        const char *h[] = {"B", "QUIT", "A", kinds[sel] == HK_SETTINGS ? "OPEN" : "PLAY"};
        ui_hints(h, 2, NULL, 0);
    }
}

static void draw_timer(void) {
    ui_title("Sleep timer");
    const char *labels[NPRESETS + 1];
    char store[NPRESETS + 1][40];
    for (int i = 0; i < NPRESETS; i++) {
        timer_short(PRESETS[i], store[i], sizeof store[i]);
        labels[i] = store[i];
    }
    char c[16];
    clock_label(clock_choice.h, clock_choice.m, c, sizeof c);
    snprintf(store[NPRESETS], sizeof store[0], "At %s", c);
    labels[NPRESETS] = store[NPRESETS];
    ui_list(PAD * 2 + ui_text_h(FS_MED), SH - PILL_H - 2 * PAD, labels, NULL, NPRESETS + 1, sel);
    if (sel == NPRESETS) {
        const char *h[] = {"B", "BACK", "A", "SET"};
        const char *l[] = {"<>", "TIME"};
        ui_hints(h, 2, l, 1);
    } else {
        const char *h[] = {"B", "BACK", "A", "SET"};
        ui_hints(h, 2, NULL, 0);
    }
}

static void fmt_time(double s, char *out, size_t n) {
    int t = s < 0 ? 0 : (int)s;
    if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void draw_player(void) {
    Uint32 now = SDL_GetTicks();
    VideoFrame *f = player_take_frame();
    if (f) {
        if (!vtex || tw != f->w || th != f->h) {
            if (vtex) SDL_DestroyTexture(vtex);
            vtex = SDL_CreateTexture(R, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->w, f->h);
            tw = f->w;
            th = f->h;
        }
        SDL_UpdateYUVTexture(vtex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
        player_frame_free(f);
    }
    PlayerState st = player_state();
    if (vtex && !player_audio_only() && st == PL_PLAYING) {
        double sa = (double)tw / th, da = (double)SW / SH;
        SDL_Rect d = {0, 0, SW, SH};
        if (sa > da) d.h = (int)(SW / sa), d.y = (SH - d.h) / 2;
        else d.w = (int)(SH * sa), d.x = (SW - d.w) / 2;
        SDL_RenderCopy(R, vtex, NULL, &d);
        static const Uint8 dim_alpha[] = {0, 80, 140, 190};
        if (cfg.dim) {
            SDL_SetRenderDrawColor(R, 0, 0, 0, dim_alpha[cfg.dim & 3]);
            SDL_RenderFillRect(R, NULL);
        }
    }
    if (fade > 0) {
        SDL_SetRenderDrawColor(R, 0, 0, 0, (Uint8)(fade * 255));
        SDL_RenderFillRect(R, NULL);
    }
    char line[300], a[32], b[32];
    if (timer_done) {
        ui_text_center(FS_MED, SH / 2 - ui_text_h(FS_MED), DIM, "Sleep timer ended");
        const char *h[] = {"B", "HOME", "A", "+15 MIN"};
        ui_hints(h, 2, NULL, 0);
        return;
    }
    if (st == PL_OPENING) ui_text_center(FS_SMALL, SH / 2 - ui_text_h(FS_SMALL) / 2, DIM, "Loading...");
    if (st == PL_ERROR) ui_text_center(FS_SMALL, SH / 2 - ui_text_h(FS_SMALL) / 2, GREY, player_error());
    if (player_audio_only() && st == PL_PLAYING && cur) {
        ui_text_center(FS_MED, SH / 2 - ui_text_h(FS_MED), DIM, "Audio only");
        snprintf(line, sizeof line, "%s S%02dE%02d", cur->series, cur->season, cur->number);
        ui_text_center(FS_SMALL, SH / 2 + PAD / 2, DIM, line);
    }
    int fading = fade > 0;
    int overlay = now < overlay_until || fading || st == PL_ERROR || menu_open;
    const Segment *seg = current_segment();
    if (!overlay) {
        if (seg) {
            const char *h[] = {"X", !strcmp(seg->type, "Recap") ? "SKIP RECAP" : "SKIP INTRO"};
            ui_hints(h, 1, NULL, 0);
        }
        return;
    }
    /* top: title and battery */
    SDL_SetRenderDrawColor(R, 0, 0, 0, 170);
    SDL_Rect topbar = {0, 0, SW, PAD * 2 + ui_text_h(FS_MED) + ui_text_h(FS_SMALL)};
    SDL_RenderFillRect(R, &topbar);
    if (cur) {
        snprintf(line, sizeof line, "%s  S%02dE%02d", cur->series, cur->season, cur->number);
        ui_text(FS_MED, PAD, PAD / 2, WHITE, line, SW - 5 * PAD);
        ui_text(FS_SMALL, PAD, PAD / 2 + ui_text_h(FS_MED), GREY, cur->name, SW - 2 * PAD);
    }
    int bat = battery_pct();
    if (bat >= 0) {
        snprintf(a, sizeof a, "%d%%", bat);
        ui_text(FS_SMALL, SW - PAD - ui_text_w(FS_SMALL, a), PAD / 2 + PAD / 4, GREY, a, 0);
    }
    /* bottom: progress and timer, above the hints */
    int hint_top = SH - PAD - (PILL_H - PAD / 2);
    int by = hint_top - PAD - ui_text_h(FS_SMALL) - PAD;
    SDL_SetRenderDrawColor(R, 0, 0, 0, 170);
    SDL_Rect bottombar = {0, by - PAD, SW, SH - by + PAD};
    SDL_RenderFillRect(R, &bottombar);
    double pos = player_position(), dur = cur ? cur->runtime : 0;
    fmt_time(pos, a, sizeof a);
    fmt_time(dur, b, sizeof b);
    snprintf(line, sizeof line, dur > 0 ? "%s / %s" : "%s", a, b);
    ui_text(FS_SMALL, PAD, by, WHITE, line, 0);
    double rem = remaining_s();
    if (ptimer.kind == 0) snprintf(line, sizeof line, "No sleep timer");
    else if (ptimer.kind == 2 && eps_left > 1) snprintf(line, sizeof line, "Sleep after %d more episodes", eps_left);
    else if (rem >= 0) {
        fmt_time(rem, a, sizeof a);
        snprintf(line, sizeof line, "Sleep in %s", a);
    } else
        snprintf(line, sizeof line, "Sleep after this episode");
    ui_text(FS_SMALL, SW - PAD - ui_text_w(FS_SMALL, line), by, GREY, line, 0);
    if (dur > 0) {
        int bw = SW - 2 * PAD, fill = (int)(bw * (pos / dur > 1 ? 1 : pos / dur));
        int y = by + ui_text_h(FS_SMALL) + PAD / 3;
        ui_round_rect(PAD, y, bw, 6, 3, (SDL_Color){70, 70, 70, 255});
        if (fill > 6) ui_round_rect(PAD, y, fill, 6, 3, WHITE);
    }
    if (menu_open) {
        static const char *labels[MI_COUNT];
        char tval[40], dval[16];
        const char *values[MI_COUNT] = {0};
        timer_short(ptimer, tval, sizeof tval);
        snprintf(dval, sizeof dval, cfg.dim ? "Level %d" : "Off", cfg.dim);
        labels[MI_TIMER] = "Sleep timer";
        values[MI_TIMER] = tval;
        labels[MI_SKIP] = seg ? (!strcmp(seg->type, "Recap") ? "Skip recap" : "Skip intro") : "Skip intro (none now)";
        labels[MI_BAN] = "Never shuffle this one (hold A)";
        labels[MI_DIM] = "Dim the picture";
        values[MI_DIM] = dval;
        labels[MI_HOME] = "Stop and go home";
        int mh = MI_COUNT * PILL_H + PAD, my = (SH - mh) / 2;
        ui_round_rect(PAD / 2, my - PAD / 2, SW - PAD, mh, 12, (SDL_Color){20, 20, 20, 240});
        ui_list(my, my + MI_COUNT * PILL_H, labels, values, MI_COUNT, menu_sel);
        if (menu_sel == MI_BAN && a_held_since) {
            int w = (int)((SW - 4 * PAD) * ((now - (Uint32)a_held_since) / (double)HOLD_MS));
            if (w > SW - 4 * PAD) w = SW - 4 * PAD;
            ui_round_rect(2 * PAD, my + MI_BAN * PILL_H + PILL_H - 8, w, 5, 2, BLACK);
        }
        const char *h[] = {"B", "CLOSE", "A", "SELECT"};
        const char *l[] = {"<>", "CHANGE"};
        ui_hints(h, 2, l, 1);
        return;
    }
    if (fading) {
        const char *h[] = {"B", "STOP", "A", "+15 MIN"};
        ui_hints(h, 2, NULL, 0);
    } else if (st == PL_ERROR) {
        const char *h[] = {"B", "HOME", "Y", "NEXT", "A", "RETRY"};
        ui_hints(h, 3, NULL, 0);
    } else {
        const char *h[] = {"B", "STOP", "Y", "NEXT", "A", player_audio_only() ? "VIDEO ON" : "VIDEO OFF"};
        const char *l[] = {"X", "MENU"};
        ui_hints(h, 3, l, 1);
    }
}

static void draw_settings(void) {
    ui_title("Settings");
    const char *labels[SI_COUNT], *values[SI_COUNT] = {0};
    char w[24], nb[16], dim[16], up[40], tsv[64], lanv[64];
    snprintf(w, sizeof w, "%d episodes", cfg.window);
    snprintf(nb, sizeof nb, "%d", bans_count());
    snprintf(dim, sizeof dim, cfg.dim ? "Level %d" : "Off", cfg.dim);
    snprintf(lanv, sizeof lanv, "%.40s", cfg.lan[0] ? cfg.lan + (strncmp(cfg.lan, "http://", 7) ? 0 : 7) : "Not set");
    snprintf(tsv, sizeof tsv, "%.40s", cfg.tailscale[0] ? cfg.tailscale + (strncmp(cfg.tailscale, "http://", 7) ? 0 : 7) : "Not set");
    snprintf(up, sizeof up, "%s", update_state == 1 ? update_tag + 8 : APP_VERSION);
    labels[SI_SHOWS] = "Shows to shuffle";
    labels[SI_WINDOW] = "Don't repeat the last";
    values[SI_WINDOW] = w;
    labels[SI_BANS] = "Never shuffled";
    values[SI_BANS] = nb;
    labels[SI_LAN] = "Home address";
    values[SI_LAN] = lanv;
    labels[SI_TS] = "Tailscale address";
    values[SI_TS] = tsv;
    labels[SI_TEST] = "Test connection";
    labels[SI_DIM] = "Dim the picture";
    values[SI_DIM] = dim;
    labels[SI_UPDATE] = update_state == 1 ? "Install update" : "Check for updates";
    values[SI_UPDATE] = up;
    labels[SI_SIGNOUT] = "Sign out (hold A)";
    values[SI_SIGNOUT] = cfg.user_name;
    ui_list(PAD * 2 + ui_text_h(FS_MED), SH - PILL_H - 3 * PAD - ui_text_h(FS_SMALL), labels, values, SI_COUNT, sel);
    if (!status[0] && sel == SI_WINDOW) snprintf(status, sizeof status, "Counted per show. Changes with left and right.");
    ui_status_line(status);
    if (sel == SI_SIGNOUT && a_held_since) {
        int bw = (int)((SW - 4 * PAD) * ((SDL_GetTicks() - (Uint32)a_held_since) / (double)HOLD_MS));
        if (bw > SW - 4 * PAD) bw = SW - 4 * PAD;
        ui_round_rect(2 * PAD, SH - PILL_H - 2 * PAD - 6, bw, 5, 2, WHITE);
    }
    int lr = sel == SI_WINDOW || sel == SI_DIM;
    const char *h[] = {"B", "BACK", "A", sel == SI_UPDATE ? (update_state == 1 ? "INSTALL" : "CHECK") : "OPEN"};
    const char *l[] = {"<>", "CHANGE"};
    ui_hints(h, (sel == SI_WINDOW || sel == SI_DIM) ? 1 : 2, l, lr ? 1 : 0);
}

static void draw_bans(void) {
    ui_title("Never shuffled");
    int n = bans_count();
    if (n == 0) {
        ui_text_center(FS_SMALL, SH / 2, GREY, "Nothing banned. Use the player menu to add one.");
        const char *h[] = {"B", "BACK"};
        ui_hints(h, 1, NULL, 0);
        return;
    }
    const char **labels = calloc((size_t)n, sizeof *labels);
    for (int i = 0; i < n; i++) labels[i] = ban_at(i)->label;
    ui_list(PAD * 2 + ui_text_h(FS_MED), SH - PILL_H - 2 * PAD, labels, NULL, n, sel);
    free(labels);
    const char *h[] = {"B", "BACK", "A", "UNBAN"};
    ui_hints(h, 2, NULL, 0);
}

/* ================================================================ per-frame logic */

static void tick_player(void) {
    Uint32 now = SDL_GetTicks();
    PlayerState st = player_state();
    if (!timer_done) {
        double rem = remaining_s();
        if (rem >= 0 && rem < FADE_SECS) {
            fade = (float)(1.0 - rem / FADE_SECS);
            float v = 1.0f - fade;
            player_set_gain(v * v);
        } else if (fade != 0) {
            fade = 0;
            player_set_gain(1);
        }
        if (rem >= 0 && rem <= 0.2 && (ptimer.kind != 2 || st == PL_PLAYING || st == PL_ENDED)) {
            logm("sleep timer ended");
            timer_done = 1;
            fade = 1;
            player_stop();
            screen_power(0);
        }
    }
    if (st == PL_ENDED && !timer_done) {
        if (ptimer.kind == 2) {
            eps_left--;
            if (eps_left <= 0) {
                logm("sleep timer ended after the last episode");
                timer_done = 1;
                fade = 1;
                player_stop();
                screen_power(0);
                return;
            }
        }
        const Episode *ep = pick_next();
        if (ep) begin_episode(ep, player_audio_only());
        else player_stop();
    }
    /* audio only: the screen goes dark once the controls have hidden */
    if (player_audio_only() && !g_blank && !menu_open && now > overlay_until && now - audio_since > AUDIO_BLANK_MS &&
        st == PL_PLAYING)
        screen_power(0);
}

/* ================================================================ main */

int main(int argc, char **argv) {
    const char *data_dir = argc > 1 ? argv[1] : ".";
    const char *log_dir = argc > 2 ? argv[2] : data_dir;
    snprintf(pak_dir, sizeof pak_dir, "%s", argc > 3 ? argv[3] : ".");
    char p[700];
    snprintf(p, sizeof p, "%s/SleepShuffle.txt", log_dir);
    g_log = fopen(p, "a");
    logm("==== Sleep Shuffle %s ====", APP_VERSION);
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGFPE, on_crash);
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    store_init(data_dir);
    home_timer = timer_decode(cfg.last_timer);
    if (home_timer.kind == 3) clock_choice = home_timer;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        logm("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_DisplayMode dm;
    int w = 640, h = 480, windowed = getenv("SS_WINDOWED") != NULL;
    if (!windowed && SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0) w = dm.w, h = dm.h;
    if (windowed && getenv("SS_SIZE")) sscanf(getenv("SS_SIZE"), "%dx%d", &w, &h);
    SDL_Window *win = SDL_CreateWindow("Sleep Shuffle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                       windowed ? 0 : SDL_WINDOW_FULLSCREEN);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_ShowCursor(SDL_DISABLE);
    ui_init(ren, w, h);
    logm("screen %dx%d, video %s", w, h, SDL_GetCurrentVideoDriver());
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    evdev_open();
    player_init();
    atexit(screen_on_at_exit);

    boot();
    int held_dir = B_NONE;
    Uint32 repeat_at = 0;
    const char *autotest = getenv("SS_AUTOTEST");
    Uint32 started = SDL_GetTicks();

    while (running) {
        Uint32 now = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int b = B_NONE, up = 0;
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
                b = map_controller(e.cbutton.button);
                up = e.type == SDL_CONTROLLERBUTTONUP;
            } else if ((e.type == SDL_KEYDOWN && !e.key.repeat) || e.type == SDL_KEYUP) {
                b = map_key(e.key.keysym.sym);
                up = e.type == SDL_KEYUP;
            }
            if (b == B_NONE) continue;
            if (up) {
                if (b == B_A) a_held_since = 0;
                if (b == held_dir) held_dir = B_NONE;
                continue;
            }
            if (sleeping_screen_off) { /* woke from a screen-off "sleep" */
                sleeping_screen_off = 0;
                screen_power(1);
                continue;
            }
            if (b == B_A) a_held_since = (int)now ? (int)now : 1;
            if (b >= B_UP && b <= B_RIGHT) {
                held_dir = b;
                repeat_at = now + 350;
            }
            press(b);
        }
        /* d-pad auto-repeat in lists */
        if (held_dir && now >= repeat_at && screen != SCR_PLAYER) {
            press(held_dir);
            repeat_at = now + 90;
        }
        /* hold-to-confirm actions */
        if (a_held_since && now - (Uint32)a_held_since >= HOLD_MS) {
            a_held_since = 0;
            settings_hold();
        }
        /* power button and lid: pause like a game, and resume on wake */
        int ev = evdev_poll();
        if (ev == 1 || ev == 2) {
            int was_playing = screen == SCR_PLAYER && cur && !timer_done &&
                              (player_state() == PL_PLAYING || player_state() == PL_OPENING);
            double pos = player_position();
            int ao = player_audio_only();
            if (was_playing) player_stop();
            logm("%s: sleeping at %.1fs", ev == 1 ? "power button" : "lid closed", pos);
            const char *sys = getenv("SYSTEM_PATH");
            char helper[400];
            struct stat st_;
            snprintf(helper, sizeof helper, "%s/bin/suspend", sys ? sys : "/mnt/SDCARD/.system/h700");
            if (stat(helper, &st_) == 0) {
                device_sleep();
                g_base[0] = 0; /* Wi-Fi may have reconnected on another network */
                if (was_playing) {
                    if (stop_at > 0 && remaining_s() <= 0) {
                        timer_done = 1;
                    } else {
                        player_play(cur->id, pos, ao, SW, SH);
                        overlay_until = SDL_GetTicks() + OVERLAY_MS;
                    }
                }
            } else {
                screen_power(0);
                sleeping_screen_off = 1;
            }
        }
        if (autotest) { /* desktop self-test driven by SS_AUTOTEST; times count from playback start */
            static int step = 0;
            static Uint32 t0;
            static int shots = 0;
            if (screen == SCR_QC && shots == 0) snprintf(shot_name, sizeof shot_name, "1-quick-connect"), shots = 1;
            if (screen == SCR_PICK && step == 0) {
                press(B_A), press(B_DOWN), press(B_A);
                snprintf(shot_name, sizeof shot_name, "2-pick-shows");
                step = 10;
            } else if (step == 10) press(B_B), step = 1;
            if (screen == SCR_HOME && step == 1) {
                const char *l[MAX_SHOWS + 4], *v[MAX_SHOWS + 4];
                int k[MAX_SHOWS + 4];
                char s[MAX_SHOWS + 4][128];
                int n = home_items(l, v, k, s);
                for (sel = 0; sel < n && k[sel] != HK_MIX; sel++) {}
                snprintf(shot_name, sizeof shot_name, "3-home");
                shots = 3;
                step = 11;
            } else if (step == 11) {
                press(B_A);
                step = 2;
                t0 = now;
            }
            Uint32 t = now - t0;
            if (getenv("SS_TIMER_SECS") && screen == SCR_PLAYER && (step == 2 || step == 3)) {
                static int ts = 0;
                if (ts == 0 && fade > 0.3f) snprintf(shot_name, sizeof shot_name, "8-fading"), logm("test: fade %.2f gain applied", fade), ts = 1;
                if (ts == 1 && timer_done) { snprintf(shot_name, sizeof shot_name, "9-timer-ended"); ts = 2; t0 = now; }
                if (ts == 2 && t > 1500) press(B_A), press(B_A), ts = 3; /* wake the screen, then +15 min */
                if (ts == 3 && t > 4000) { logm("test: after +15 timer_done=%d remaining=%.0fs state=%d", timer_done, remaining_s(), player_state()); running = 0; }
                step = 2;
                goto autotest_done;
            }
            if (step == 2 && t > 1500 && shots == 3) snprintf(shot_name, sizeof shot_name, "4-player-overlay"), shots = 4;
            if (step == 2 && t > 4000 && shots == 4) snprintf(shot_name, sizeof shot_name, "5-player-intro-pill"), shots = 5;
            if (step == 2 && t > 5000) press(B_A), press(B_A), step = 3;  /* reveal, then video off */
            if (step == 3 && t > 11000) press(B_A), press(B_A), step = 4; /* wake, then video on */
            if (step == 4 && t > 15000) press(B_A), press(B_X), step = 5; /* reveal, menu */
            if (step == 5 && shots == 5) snprintf(shot_name, sizeof shot_name, "6-player-menu"), shots = 6;
            if (step == 5 && t > 16000) press(B_A), step = 6;             /* skip intro if still in it */
            if (step == 6 && t > 18000) press(B_A), press(B_Y), step = 7; /* next episode */
            if (step == 7 && t > 22000) press(B_A), press(B_B), step = 8; /* stop, home */
            if (step == 8 && shots == 6) {
                press(B_DOWN);
                for (int i = 0; i < 9; i++) press(B_DOWN); /* to Settings */
                shots = 7;
            }
            if (step == 8 && shots == 7 && screen == SCR_HOME) { press(B_A); shots = 8; }
            if (step == 8 && shots == 8 && screen == SCR_SETTINGS) { snprintf(shot_name, sizeof shot_name, "7-settings"); shots = 9; }
            if (step == 8 && t > 24000) running = 0;
            if (now - started > 60000) running = 0;
        }
    autotest_done:
        if (screen == SCR_QC && now >= qc_next) {
            qc_next = now + 3000;
            int r = jf_qc_poll(qc_secret);
            if (r == 1) {
                if (jf_qc_finish(qc_secret) == 0) {
                    if (cfg.nshows == 0) open_picker(SCR_HOME);
                    else go(SCR_HOME);
                } else
                    message("Sign-in failed", "Jellyfin turned down the Quick Connect code. Try again.", SCR_BOOT);
            }
        }
        if (screen == SCR_PLAYER) tick_player();

        ui_clear();
        switch (screen) {
        case SCR_KEYBOARD: draw_keyboard(); break;
        case SCR_QC: draw_qc(); break;
        case SCR_PICK: draw_pick(); break;
        case SCR_HOME: draw_home(); break;
        case SCR_TIMER: draw_timer(); break;
        case SCR_PLAYER: draw_player(); break;
        case SCR_SETTINGS: draw_settings(); break;
        case SCR_BANS: draw_bans(); break;
        case SCR_MESSAGE: draw_message(); break;
        default: break;
        }
        if (shot_name[0] && getenv("SS_SHOTS")) {
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, SW, SH, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
            char path[600];
            snprintf(path, sizeof path, "%s/%s.bmp", getenv("SS_SHOTS"), shot_name);
            SDL_SaveBMP(sf, path);
            SDL_FreeSurface(sf);
            shot_name[0] = 0;
        }
        SDL_RenderPresent(R);
        SDL_Delay(g_blank ? 40 : 2);
    }

    logm("quitting (exit code %d)", exit_code);
    player_stop();
    player_shutdown();
    screen_power(1);
    SDL_Quit();
    if (g_log) fclose(g_log);
    return exit_code;
}
