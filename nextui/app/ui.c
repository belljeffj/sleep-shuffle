/* Drawing in NextUI's style. Text uses NextUI's own font through the SDL2_ttf that NextUI ships,
 * loaded at run time; if either is missing, a font baked in at build time is used instead. */
#define _GNU_SOURCE
#include "ui.h"

#include <dlfcn.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "font.h"

SDL_Renderer *R;
int SW = 640, SH = 480, PAD = 20, PILL_H = 54;
const SDL_Color WHITE = {255, 255, 255, 255}, BLACK = {0, 0, 0, 255}, GREY = {150, 150, 150, 255},
                DIM = {95, 95, 95, 255};

/* ---------------------------------------------------------------- SDL2_ttf via dlopen */

typedef struct _TTF_Font TTF_Font;
static int (*pTTF_Init)(void);
static TTF_Font *(*pTTF_OpenFont)(const char *, int);
static SDL_Surface *(*pTTF_RenderUTF8_Blended)(TTF_Font *, const char *, SDL_Color);
static int (*pTTF_SizeUTF8)(TTF_Font *, const char *, int *, int *);
static int (*pTTF_FontHeight)(const TTF_Font *);
static TTF_Font *ttf[FS_COUNT];
static int px[FS_COUNT];

static const BakedFont *baked[FS_COUNT] = {&font_small, &font_med, &font_big};
static SDL_Texture *glyph[FS_COUNT][95];

static void try_ttf(void) {
    void *h = dlopen("libSDL2_ttf-2.0.so.0", RTLD_NOW);
    if (!h) {
        logm("SDL2_ttf not available; using the built-in font");
        return;
    }
    pTTF_Init = dlsym(h, "TTF_Init");
    pTTF_OpenFont = dlsym(h, "TTF_OpenFont");
    pTTF_RenderUTF8_Blended = dlsym(h, "TTF_RenderUTF8_Blended");
    pTTF_SizeUTF8 = dlsym(h, "TTF_SizeUTF8");
    pTTF_FontHeight = dlsym(h, "TTF_FontHeight");
    if (!pTTF_Init || !pTTF_OpenFont || !pTTF_RenderUTF8_Blended || !pTTF_SizeUTF8 || !pTTF_FontHeight ||
        pTTF_Init() != 0)
        return;
    const char *sd = getenv("SDCARD_PATH") ? getenv("SDCARD_PATH") : "/mnt/SDCARD";
    char pat[300], found[400] = "";
    const char *names[] = {"font1.ttf", "font2.ttf", "*.ttf", "*.otf"};
    for (size_t i = 0; i < sizeof names / sizeof *names && !found[0]; i++) {
        glob_t g;
        snprintf(pat, sizeof pat, "%s/.system/res/%s", sd, names[i]);
        if (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc) snprintf(found, sizeof found, "%s", g.gl_pathv[0]);
        globfree(&g);
    }
    if (!found[0]) {
        logm("no NextUI font found under %s/.system/res; using the built-in font", sd);
        return;
    }
    for (int i = 0; i < FS_COUNT; i++) ttf[i] = pTTF_OpenFont(found, px[i]);
    if (!ttf[0] || !ttf[1] || !ttf[2]) {
        logm("could not open %s; using the built-in font", found);
        ttf[0] = ttf[1] = ttf[2] = NULL;
        return;
    }
    logm("font: %s", found);
}

/* ---------------------------------------------------------------- text cache */

#define CACHE 160
static struct {
    int size;
    char *s;
    SDL_Texture *t;
    int w, h;
} cache[CACHE];
static int ncache;

static SDL_Texture *cached(int size, const char *s, int *w, int *h) {
    for (int i = 0; i < ncache; i++)
        if (cache[i].size == size && !strcmp(cache[i].s, s)) {
            *w = cache[i].w;
            *h = cache[i].h;
            return cache[i].t;
        }
    if (ncache == CACHE) {
        for (int i = 0; i < ncache; i++) {
            SDL_DestroyTexture(cache[i].t);
            free(cache[i].s);
        }
        ncache = 0;
    }
    SDL_Surface *surf = pTTF_RenderUTF8_Blended(ttf[size], s, WHITE);
    if (!surf) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, surf);
    *w = surf->w;
    *h = surf->h;
    SDL_FreeSurface(surf);
    cache[ncache].size = size;
    cache[ncache].s = strdup(s);
    cache[ncache].t = t;
    cache[ncache].w = *w;
    cache[ncache].h = *h;
    ncache++;
    return t;
}

void ui_init(SDL_Renderer *r, int w, int h) {
    R = r;
    SW = w;
    SH = h;
    double s = h / 240.0; /* NextUI's layout is defined at 240 lines and scaled */
    PAD = (int)(10 * s);
    PILL_H = (int)(27 * s);
    px[FS_SMALL] = (int)(12 * s);
    px[FS_MED] = (int)(16 * s);
    px[FS_BIG] = (int)(32 * s);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    try_ttf();
    if (!ttf[0]) {
        for (int f = 0; f < FS_COUNT; f++)
            for (int c = 0; c < 95; c++) {
                const Glyph *g = &baked[f]->glyphs[c];
                SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, g->w, g->h, 32, SDL_PIXELFORMAT_ARGB8888);
                Uint32 *p = sf->pixels;
                for (int y = 0; y < g->h; y++)
                    for (int x = 0; x < g->w; x++)
                        p[y * (sf->pitch / 4) + x] = ((Uint32)g->alpha[y * g->w + x] << 24) | 0xFFFFFF;
                glyph[f][c] = SDL_CreateTextureFromSurface(R, sf);
                SDL_SetTextureBlendMode(glyph[f][c], SDL_BLENDMODE_BLEND);
                SDL_FreeSurface(sf);
            }
    }
}

/* fallback renderer: ASCII glyphs; each non-ASCII UTF-8 character shows as '?' */
static int baked_draw(int size, int x, int y, SDL_Color c, const char *s, int draw) {
    const BakedFont *f = baked[size];
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int ch = *p;
        if (ch >= 0x80) {
            if ((ch & 0xC0) == 0x80) continue;
            ch = '?';
        }
        if (ch < 32 || ch > 126) ch = '?';
        const Glyph *g = &f->glyphs[ch - 32];
        if (draw) {
            SDL_SetTextureColorMod(glyph[size][ch - 32], c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(glyph[size][ch - 32], c.a);
            SDL_Rect d = {x + w, y, g->w, g->h};
            SDL_RenderCopy(R, glyph[size][ch - 32], NULL, &d);
        }
        w += g->advance;
    }
    return w;
}

int ui_text_h(int size) { return ttf[size] ? pTTF_FontHeight(ttf[size]) : baked[size]->height; }

int ui_text_w(int size, const char *s) {
    if (!s || !*s) return 0;
    if (ttf[size]) {
        int w = 0, h = 0;
        pTTF_SizeUTF8(ttf[size], s, &w, &h);
        return w;
    }
    return baked_draw(size, 0, 0, WHITE, s, 0);
}

int ui_text(int size, int x, int y, SDL_Color c, const char *s, int max_w) {
    if (!s || !*s) return 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", s);
    if (max_w > 0 && ui_text_w(size, buf) > max_w) {
        size_t n = strlen(buf);
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--; /* stay on a character boundary */
            buf[n] = 0;
            char t[520];
            snprintf(t, sizeof t, "%s...", buf);
            if (ui_text_w(size, t) <= max_w) {
                snprintf(buf, sizeof buf, "%s", t);
                break;
            }
        }
    }
    if (!ttf[size]) return baked_draw(size, x, y, c, buf, 1);
    int w, h;
    SDL_Texture *t = cached(size, buf, &w, &h);
    if (!t) return 0;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_Rect d = {x, y, w, h};
    SDL_RenderCopy(R, t, NULL, &d);
    return w;
}

void ui_text_center(int size, int y, SDL_Color c, const char *s) {
    int w = ui_text_w(size, s);
    if (w > SW - 2 * PAD) w = SW - 2 * PAD;
    ui_text(size, (SW - w) / 2, y, c, s, SW - 2 * PAD);
}

void ui_round_rect(int x, int y, int w, int h, int r, SDL_Color c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    SDL_Rect mid = {x, y + r, w, h - 2 * r};
    SDL_RenderFillRect(R, &mid);
    for (int i = 0; i < r; i++) { /* rounded top and bottom bands, one line at a time */
        double dy = r - i - 0.5;
        int dx = (int)(r - SDL_sqrt((double)r * r - dy * dy) + 0.5);
        SDL_Rect top = {x + dx, y + i, w - 2 * dx, 1}, bot = {x + dx, y + h - 1 - i, w - 2 * dx, 1};
        SDL_RenderFillRect(R, &top);
        SDL_RenderFillRect(R, &bot);
    }
}

void ui_clear(void) {
    SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
    SDL_RenderClear(R);
}

void ui_row(int y, const char *label, const char *value, int selected) {
    int th = ui_text_h(FS_MED);
    int ty = y + (PILL_H - th) / 2;
    int vw = value && *value ? ui_text_w(FS_MED, value) : 0;
    int max_label = SW - 4 * PAD - (vw ? vw + PAD : 0);
    if (selected) {
        int lw = ui_text_w(FS_MED, label);
        if (lw > max_label) lw = max_label;
        int pw = lw + 2 * PAD;
        if (vw) pw = SW - 2 * PAD; /* rows with a value span the width, like NextUI settings */
        ui_round_rect(PAD, y, pw, PILL_H, PILL_H / 2, WHITE);
        ui_text(FS_MED, 2 * PAD, ty, BLACK, label, max_label);
        if (vw) ui_text(FS_MED, SW - 2 * PAD - vw, ty, BLACK, value, 0);
    } else {
        ui_text(FS_MED, 2 * PAD, ty, WHITE, label, max_label);
        if (vw) ui_text(FS_MED, SW - 2 * PAD - vw, ty, GREY, value, 0);
    }
}

int ui_list(int top, int bottom, const char *const *labels, const char *const *values, int n, int sel) {
    int fit = (bottom - top) / PILL_H;
    if (fit < 1) fit = 1;
    int first = 0;
    if (sel >= fit) first = sel - fit + 1;
    for (int i = 0; i < fit && first + i < n; i++)
        ui_row(top + i * PILL_H, labels[first + i], values ? values[first + i] : NULL, first + i == sel);
    if (n > fit) { /* scroll position marks */
        int bh = bottom - top, kh = bh * fit / n, ky = top + (bh - kh) * first / (n - fit);
        ui_round_rect(SW - PAD / 2 - 3, ky, 4, kh, 2, DIM);
    }
    return fit;
}

static int hint_group_w(const char *const *pairs, int n) {
    int bh = PILL_H - PAD / 2, w = PAD / 2;
    for (int i = 0; i < n; i++) {
        int bw = ui_text_w(FS_SMALL, pairs[2 * i]) + PAD;
        if (bw < bh - PAD / 2) bw = bh - PAD / 2;
        w += bw + PAD / 3 + ui_text_w(FS_SMALL, pairs[2 * i + 1]) + PAD / 2;
    }
    return w;
}

static void hint_group(int x, int y, const char *const *pairs, int n) {
    int h = PILL_H - PAD / 2, w = hint_group_w(pairs, n);
    ui_round_rect(x, y, w, h, h / 2, (SDL_Color){40, 40, 40, 235});
    int cx = x + PAD / 2, inner = h - PAD / 2, th = ui_text_h(FS_SMALL);
    for (int i = 0; i < n; i++) {
        const char *b = pairs[2 * i], *l = pairs[2 * i + 1];
        int bw = ui_text_w(FS_SMALL, b) + PAD;
        if (bw < inner) bw = inner;
        ui_round_rect(cx, y + PAD / 4, bw, inner, inner / 2, (SDL_Color){220, 220, 220, 255});
        ui_text(FS_SMALL, cx + (bw - ui_text_w(FS_SMALL, b)) / 2, y + (h - th) / 2, (SDL_Color){30, 30, 30, 255}, b, 0);
        cx += bw + PAD / 3;
        cx += ui_text(FS_SMALL, cx, y + (h - th) / 2, WHITE, l, 0) + PAD / 2;
    }
}

void ui_hints(const char *const *right, int nright, const char *const *left, int nleft) {
    int y = SH - PAD - (PILL_H - PAD / 2);
    if (nleft) hint_group(PAD, y, left, nleft);
    if (nright) hint_group(SW - PAD - hint_group_w(right, nright), y, right, nright);
}

void ui_title(const char *title) {
    ui_text(FS_MED, PAD + PAD / 2, PAD, GREY, title, SW - 3 * PAD);
}

void ui_status_line(const char *s) {
    if (!s || !*s) return;
    int y = SH - PAD - (PILL_H - PAD / 2) - ui_text_h(FS_SMALL) - PAD / 2;
    ui_text(FS_SMALL, 2 * PAD, y, GREY, s, SW - 4 * PAD);
}
