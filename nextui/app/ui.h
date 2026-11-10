#pragma once
#include <SDL.h>

enum { FS_SMALL, FS_MED, FS_BIG, FS_COUNT };

extern SDL_Renderer *R;
extern int SW, SH, PAD, PILL_H;

void ui_init(SDL_Renderer *r, int w, int h);
int ui_text_w(int size, const char *s);
int ui_text_h(int size);
/* draws s (UTF-8) at x,y; truncates with "..." to max_w if > 0; returns width drawn */
int ui_text(int size, int x, int y, SDL_Color c, const char *s, int max_w);
void ui_text_center(int size, int y, SDL_Color c, const char *s);
void ui_round_rect(int x, int y, int w, int h, int r, SDL_Color c);
void ui_clear(void);

/* NextUI-style list row: white pill when selected; optional value text on the right */
void ui_row(int y, const char *label, const char *value, int selected);
/* list with scrolling; draws rows from items[] between top and bottom. Returns rows that fit. */
int ui_list(int top, int bottom, const char *const *labels, const char *const *values, int n, int sel);
/* button hints along the bottom: pairs of {button, label}, right-aligned; left pairs drawn on the left */
void ui_hints(const char *const *right, int nright, const char *const *left, int nleft);
void ui_title(const char *title);
void ui_status_line(const char *s); /* small grey line just above the hints */

extern const SDL_Color WHITE, BLACK, GREY, DIM;
