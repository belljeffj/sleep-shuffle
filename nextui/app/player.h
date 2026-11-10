#pragma once
#include <SDL.h>

typedef enum { PL_IDLE, PL_OPENING, PL_PLAYING, PL_ENDED, PL_ERROR } PlayerState;

/* frame handed to the UI thread; free with player_frame_free */
typedef struct {
    int w, h;
    unsigned char *data[3];
    int linesize[3];
    void *priv;
} VideoFrame;

int player_init(void);
void player_shutdown(void);
/* Starts item_id at pos seconds. A fresh server stream is opened every time (no seeking inside a stream). */
void player_play(const char *item_id, double pos, int audio_only, int screen_w, int screen_h);
void player_stop(void);
PlayerState player_state(void);
const char *player_error(void);
double player_position(void); /* seconds into the episode */
int player_audio_only(void);
void player_set_gain(float g); /* 0..1, applied to audio as it is queued */
/* Newest frame due by now, or NULL. */
VideoFrame *player_take_frame(void);
void player_frame_free(VideoFrame *f);
const char *player_route(void);
