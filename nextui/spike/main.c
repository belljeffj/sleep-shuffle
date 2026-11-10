/* Sleep Shuffle test pak for NextUI on Anbernic H700 (RG SP).
 *
 * A throwaway spike, not the real app. It answers three questions on the device:
 *   1. Does Jellyfin Quick Connect sign-in work from the handheld?
 *   2. Can the H700 decode a 640x480 H.264 stream from Jellyfin smoothly, and how warm does it get?
 *   3. Does Jellyfin serve an audio-only stream for a TV episode, so "video off" saves battery?
 * Everything it learns goes to SleepShuffleTest.txt in NextUI's logs folder.
 *
 * Controls (as SDL reports them; the log records what each physical button sends):
 *   A: switch video / audio only    X: show stats    Y: next random episode    B or START: quit
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <execinfo.h>
#include <glob.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "net.h"

#define APP_VERSION "0.1.1-test"
#define RING 8
#define OUT_RATE 48000

/* ---------------------------------------------------------------- logging */

static FILE *g_log;

static void logm(const char *fmt, ...) {
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
    fsync(fileno(g_log)); /* the card may lose cached writes if the power button is held */
}

/* On a crash, write the signal and a backtrace to the log before dying. Addresses can be
 * turned into source lines with addr2line against the same binary from the release. */
static void on_crash(int sig) {
    if (g_log) {
        int fd = fileno(g_log);
        char head[64];
        int n = snprintf(head, sizeof head, "CRASH: signal %d, backtrace:\n", sig);
        if (write(fd, head, (size_t)n) < 0) {}
        void *frames[48];
        int k = backtrace(frames, 48);
        backtrace_symbols_fd(frames, k, fd);
        fsync(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* If the main loop stops ticking for 20 s, give the device back to NextUI. */
static volatile Uint32 g_heartbeat;
static int watchdog(void *arg) {
    (void)arg;
    for (;;) {
        SDL_Delay(1000);
        Uint32 hb = g_heartbeat;
        if (hb && SDL_GetTicks() - hb > 20000) {
            logm("WATCHDOG: main loop stuck for 20 s, exiting");
            _exit(3);
        }
    }
    return 0;
}

#define STEP(...) logm("step: " __VA_ARGS__)

static void ff_log(void *avcl, int level, const char *fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_WARNING || !g_log) return;
    char line[512];
    vsnprintf(line, sizeof line, fmt, vl);
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = 0;
    logm("ffmpeg: %s", line);
}

/* ---------------------------------------------------------------- config (outside the pak) */

typedef struct {
    char server[256], token[128], user_id[64], user_name[128], device_id[64];
} Config;

static Config cfg;
static char cfg_path[1024];

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '/')) s[--n] = 0;
}

static void cfg_load(void) {
    FILE *f = fopen(cfg_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *v = eq + 1;
        trim(v);
#define K(name) if (!strcmp(line, #name)) snprintf(cfg.name, sizeof cfg.name, "%s", v);
        K(server) K(token) K(user_id) K(user_name) K(device_id)
#undef K
    }
    fclose(f);
}

/* Accepts "192.168.86.20:8096" or a full URL; stores "http://host:port" with no trailing slash. */
static void cfg_normalize_server(void) {
    trim(cfg.server);
    if (cfg.server[0] && strncmp(cfg.server, "http", 4) != 0) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "http://%.240s", cfg.server);
        snprintf(cfg.server, sizeof cfg.server, "%s", tmp);
    }
}

static void cfg_save(void) {
    FILE *f = fopen(cfg_path, "w");
    if (!f) {
        logm("could not write %s", cfg_path);
        return;
    }
    fprintf(f, "# Sleep Shuffle test pak. Put your Jellyfin home address after server=, e.g. 192.168.86.20:8096\n");
    fprintf(f, "server=%s\ntoken=%s\nuser_id=%s\nuser_name=%s\ndevice_id=%s\n", cfg.server, cfg.token, cfg.user_id,
            cfg.user_name, cfg.device_id);
    fclose(f);
}

static void auth_header(char *out, size_t n) {
    snprintf(out, n,
             "MediaBrowser Client=\"Sleep Shuffle\", Device=\"Anbernic RG SP\", DeviceId=\"%s\", Version=\"%s\"%s%s%s",
             cfg.device_id, APP_VERSION, cfg.token[0] ? ", Token=\"" : "", cfg.token, cfg.token[0] ? "\"" : "");
}

static int jf(const char *method, const char *path, const char *body, HttpResp *r) {
    char url[1024], auth[512];
    snprintf(url, sizeof url, "%s%s", cfg.server, path);
    auth_header(auth, sizeof auth);
    Uint32 t0 = SDL_GetTicks();
    int rc = http_request(method, url, auth, body, 6000, r);
    logm("%s %s -> rc=%d status=%d (%u ms)", method, path, rc, r->status, SDL_GetTicks() - t0);
    return rc;
}

/* ---------------------------------------------------------------- device helpers */

static int read_int_file(const char *pattern) {
    glob_t g;
    int v = -1;
    if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        FILE *f = fopen(g.gl_pathv[0], "r");
        if (f) {
            if (fscanf(f, "%d", &v) != 1) v = -1;
            fclose(f);
        }
    }
    globfree(&g);
    return v;
}

static char g_blpath[512];

/* Screen off for audio-only. Uses the kernel's backlight power switch if the device has one. */
static void backlight(int on) {
    if (!g_blpath[0]) {
        glob_t g;
        if (glob("/sys/class/backlight/*/bl_power", 0, NULL, &g) == 0 && g.gl_pathc > 0)
            snprintf(g_blpath, sizeof g_blpath, "%s", g.gl_pathv[0]);
        globfree(&g);
        if (!g_blpath[0]) snprintf(g_blpath, sizeof g_blpath, "none");
        logm("backlight control: %s", g_blpath);
    }
    if (!strcmp(g_blpath, "none")) return;
    FILE *f = fopen(g_blpath, "w");
    if (!f) {
        logm("backlight: cannot open %s", g_blpath);
        return;
    }
    fprintf(f, "%d", on ? 0 : 4);
    fclose(f);
    logm("backlight %s", on ? "on" : "off");
}

static void restore_backlight(void) { backlight(1); }

/* ---------------------------------------------------------------- text */

static SDL_Renderer *ren;
static SDL_Texture *glyph_tex[2][95];

static void fonts_init(void) {
    const BakedFont *fonts[2] = {&font_small, &font_big};
    for (int f = 0; f < 2; f++) {
        for (int c = 0; c < 95; c++) {
            const Glyph *g = &fonts[f]->glyphs[c];
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, g->w, g->h, 32, SDL_PIXELFORMAT_ARGB8888);
            Uint32 *px = s->pixels;
            for (int y = 0; y < g->h; y++)
                for (int x = 0; x < g->w; x++)
                    px[y * (s->pitch / 4) + x] = ((Uint32)g->alpha[y * g->w + x] << 24) | 0xFFFFFF;
            glyph_tex[f][c] = SDL_CreateTextureFromSurface(ren, s);
            SDL_SetTextureBlendMode(glyph_tex[f][c], SDL_BLENDMODE_BLEND);
            SDL_FreeSurface(s);
        }
    }
}

static int text_w(int big, const char *s) {
    const BakedFont *f = big ? &font_big : &font_small;
    int w = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        w += f->glyphs[c - 32].advance;
    }
    return w;
}

static void text(int big, int x, int y, Uint8 grey, const char *s) {
    const BakedFont *f = big ? &font_big : &font_small;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const Glyph *g = &f->glyphs[c - 32];
        SDL_Texture *t = glyph_tex[big][c - 32];
        SDL_SetTextureColorMod(t, grey, grey, grey);
        SDL_Rect r = {x, y, g->w, g->h};
        SDL_RenderCopy(ren, t, NULL, &r);
        x += g->advance;
    }
}

static int SW = 640, SH = 480;

static void text_c(int big, int y, Uint8 grey, const char *s) { text(big, (SW - text_w(big, s)) / 2, y, grey, s); }

static void pill(int y, const char *s) { /* NextUI-ish hint pill */
    int w = text_w(0, s) + 28;
    SDL_Rect r = {(SW - w) / 2, y, w, font_small.height + 12};
    SDL_SetRenderDrawColor(ren, 40, 40, 40, 255);
    SDL_RenderFillRect(ren, &r);
    text(0, r.x + 14, y + 6, 220, s);
}

/* ---------------------------------------------------------------- player */

typedef struct {
    SDL_mutex *mx;
    SDL_cond *cv;
    SDL_AudioDeviceID dev;
    /* requests from the UI */
    int quit, reopen;
    int audio_only;
    double start_pos;
    char item_id[64];
    char play_session[40];
    /* status */
    int state; /* 0 idle, 1 opening, 2 playing, 3 ended, 4 error */
    char msg[256], route[48];
    /* decoded frames waiting to be shown */
    AVFrame *ring[RING];
    double ring_pts[RING];
    int head, count;
    /* clock */
    int has_audio;
    double audio_end; /* stream time at the end of the queued audio */
    /* stats */
    long decoded, dropped, shown, audio_underruns;
    double decode_ms;
    int vw, vh;
    char vcodec[24], acodec[24];
} Player;

static Player P;
static char next_item[64]; /* chosen by the UI, handed to the decoder in request_play */

static double queued_secs(void) { return SDL_GetQueuedAudioSize(P.dev) / (double)(OUT_RATE * 4); }

static double clock_now(void) { return P.audio_end - queued_secs(); }

static int interrupt_cb(void *opaque) {
    (void)opaque;
    return P.quit || P.reopen;
}

static void ring_clear(void) {
    while (P.count) {
        av_frame_free(&P.ring[P.head]);
        P.head = (P.head + 1) % RING;
        P.count--;
    }
}

static void new_session(void) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) P.play_session[i] = hex[rand() % 16];
    P.play_session[32] = 0;
}

static void stream_url(char *url, size_t n, int audio_route) {
    long long ticks = (long long)(P.start_pos * 10000000.0);
    if (audio_route)
        snprintf(url, n,
                 "%s/Audio/%s/stream.mp3?container=mp3&audioCodec=mp3&audioBitRate=128000&audioChannels=2"
                 "&deviceId=%s&playSessionId=%s&startTimeTicks=%lld&api_key=%s",
                 cfg.server, P.item_id, cfg.device_id, P.play_session, ticks, cfg.token);
    else
        snprintf(url, n,
                 "%s/Videos/%s/master.m3u8?mediaSourceId=%s&deviceId=%s&playSessionId=%s"
                 "&videoCodec=h264&audioCodec=aac&maxAudioChannels=2&maxWidth=640&maxHeight=480"
                 "&videoBitrate=1200000&audioBitrate=128000&segmentContainer=ts&api_key=%s",
                 cfg.server, P.item_id, P.item_id, cfg.device_id, P.play_session, cfg.token);
}

static void stop_server_transcode(void) {
    char path[256];
    HttpResp r;
    snprintf(path, sizeof path, "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s", cfg.device_id,
             P.play_session);
    if (jf("DELETE", path, NULL, &r) == 0) http_free(&r);
}

/* One open-decode cycle. Returns when the stream ends, fails, or the UI asks to reopen or quit. */
static void play_once(void) {
    int audio_only = P.audio_only;
    double start = P.start_pos;
    AVFormatContext *fmt = NULL;
    AVCodecContext *vctx = NULL, *actx = NULL;
    SwrContext *swr = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int vi = -1, ai = -1, ok = 0;
    double offset = 0; /* added to stream timestamps */
    char url[2048];

    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        int audio_route = audio_only && attempt == 0;
        stream_url(url, sizeof url, audio_route);
        snprintf(P.route, sizeof P.route, "%s", audio_only ? (audio_route ? "audio endpoint (mp3)"
                                                                         : "video stream, video discarded")
                                                           : "video stream (HLS)");
        logm("opening %s at %.1fs", P.route, start);
        fmt = avformat_alloc_context();
        fmt->interrupt_callback.callback = interrupt_cb;
        AVDictionary *o = NULL;
        av_dict_set(&o, "rw_timeout", "15000000", 0);
        av_dict_set(&o, "reconnect", "1", 0);
        av_dict_set(&o, "probesize", "1000000", 0);
        av_dict_set(&o, "analyzeduration", "3000000", 0);
        Uint32 t0 = SDL_GetTicks();
        int rc = avformat_open_input(&fmt, url, NULL, &o);
        av_dict_free(&o);
        if (rc < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
            char e[128];
            av_strerror(rc, e, sizeof e);
            logm("open failed (%s) after %u ms", e, SDL_GetTicks() - t0);
            avformat_close_input(&fmt);
            if (P.quit || P.reopen) goto out;
            continue;
        }
        logm("opened in %u ms: %s, %u streams", SDL_GetTicks() - t0, fmt->iformat->name, fmt->nb_streams);
        ok = 1;
        if (audio_route) offset = start;
        else if (start > 1.0) {
            int64_t ts = (int64_t)(start * AV_TIME_BASE);
            int s = avformat_seek_file(fmt, -1, INT64_MIN, ts, INT64_MAX, 0);
            logm("seek to %.1fs: %s", start, s < 0 ? "failed" : "ok");
        }
    }
    if (!ok) {
        snprintf(P.msg, sizeof P.msg, "Jellyfin wouldn't send the stream. See the log.");
        P.state = 4;
        goto out;
    }

    ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (!audio_only) vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if ((int)i != vi && (int)i != ai) fmt->streams[i]->discard = AVDISCARD_ALL;

    if (vi >= 0) {
        AVStream *st = fmt->streams[vi];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        vctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(vctx, st->codecpar);
        vctx->thread_count = 4;
        vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (!c || avcodec_open2(vctx, c, NULL) < 0) {
            logm("no video decoder");
            avcodec_free_context(&vctx);
            vi = -1;
        } else {
            P.vw = vctx->width;
            P.vh = vctx->height;
            snprintf(P.vcodec, sizeof P.vcodec, "%s", c->name);
            logm("video: %s %dx%d %s", c->name, vctx->width, vctx->height, av_get_pix_fmt_name(vctx->pix_fmt));
        }
    }
    if (ai >= 0) {
        AVStream *st = fmt->streams[ai];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        actx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(actx, st->codecpar);
        if (!c || avcodec_open2(actx, c, NULL) < 0) {
            logm("no audio decoder");
            avcodec_free_context(&actx);
            ai = -1;
        } else {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, OUT_RATE, &actx->ch_layout, actx->sample_fmt,
                                actx->sample_rate, 0, NULL);
            swr_init(swr);
            snprintf(P.acodec, sizeof P.acodec, "%s", c->name);
            logm("audio: %s %d Hz %d ch", c->name, actx->sample_rate, actx->ch_layout.nb_channels);
        }
    }

    SDL_LockMutex(P.mx);
    P.has_audio = ai >= 0;
    P.audio_end = start;
    P.state = 2;
    P.msg[0] = 0;
    SDL_UnlockMutex(P.mx);
    SDL_ClearQueuedAudio(P.dev);
    SDL_PauseAudioDevice(P.dev, 0);

    double wall_base = av_gettime_relative() / 1e6 - start; /* clock when there is no audio */

    while (!P.quit && !P.reopen) {
        /* Don't run far ahead of the speaker. */
        if (P.has_audio && queued_secs() > 1.5) {
            SDL_Delay(10);
            continue;
        }
        int rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            if (rc == AVERROR_EOF || (fmt->pb && avio_feof(fmt->pb))) {
                logm("end of stream");
                /* let the queued audio play out */
                while (!P.quit && !P.reopen && P.has_audio && queued_secs() > 0.05) SDL_Delay(50);
                if (!P.quit && !P.reopen) P.state = 3;
            } else if (!P.quit && !P.reopen) {
                char e[128];
                av_strerror(rc, e, sizeof e);
                logm("read error: %s", e);
                snprintf(P.msg, sizeof P.msg, "The stream dropped (%s).", e);
                P.state = 4;
            }
            break;
        }
        AVStream *st = fmt->streams[pkt->stream_index];
        if (pkt->stream_index == ai && actx) {
            if (avcodec_send_packet(actx, pkt) == 0) {
                while (avcodec_receive_frame(actx, fr) == 0) {
                    int64_t ts = fr->best_effort_timestamp;
                    double pts = ts == AV_NOPTS_VALUE ? -1 : ts * av_q2d(st->time_base) + offset;
                    int max = swr_get_out_samples(swr, fr->nb_samples);
                    uint8_t *buf = NULL;
                    av_samples_alloc(&buf, NULL, 2, max, AV_SAMPLE_FMT_S16, 0);
                    int n = swr_convert(swr, &buf, max, (const uint8_t **)fr->extended_data, fr->nb_samples);
                    if (n > 0) {
                        if (queued_secs() < 0.02 && P.audio_end - start > 2.0) P.audio_underruns++; /* ignore the start */
                        SDL_QueueAudio(P.dev, buf, (Uint32)n * 4);
                        SDL_LockMutex(P.mx);
                        if (pts >= 0 && P.audio_end <= start + 0.001) P.audio_end = pts; /* first frame */
                        P.audio_end += n / (double)OUT_RATE;
                        SDL_UnlockMutex(P.mx);
                    }
                    av_freep(&buf);
                }
            }
        } else if (pkt->stream_index == vi && vctx) {
            Uint32 t0 = SDL_GetTicks();
            if (avcodec_send_packet(vctx, pkt) == 0) {
                while (avcodec_receive_frame(vctx, fr) == 0) {
                    P.decoded++;
                    P.decode_ms += SDL_GetTicks() - t0;
                    t0 = SDL_GetTicks();
                    int64_t ts = fr->best_effort_timestamp;
                    double pts = ts == AV_NOPTS_VALUE ? 0 : ts * av_q2d(st->time_base) + offset;
                    double now = P.has_audio ? clock_now() : av_gettime_relative() / 1e6 - wall_base;
                    if (pts < now - 0.12) { /* too late to be worth showing */
                        P.dropped++;
                        av_frame_unref(fr);
                        continue;
                    }
                    AVFrame *keep = av_frame_alloc();
                    if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P) {
                        sws = sws_getCachedContext(sws, fr->width, fr->height, fr->format, fr->width, fr->height,
                                                   AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
                        keep->format = AV_PIX_FMT_YUV420P;
                        keep->width = fr->width;
                        keep->height = fr->height;
                        av_frame_get_buffer(keep, 32);
                        sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, keep->data,
                                  keep->linesize);
                    } else {
                        av_frame_move_ref(keep, fr);
                    }
                    av_frame_unref(fr);
                    SDL_LockMutex(P.mx);
                    while (P.count == RING && !P.quit && !P.reopen) SDL_CondWaitTimeout(P.cv, P.mx, 50);
                    if (P.count < RING) {
                        int slot = (P.head + P.count) % RING;
                        P.ring[slot] = keep;
                        P.ring_pts[slot] = pts;
                        P.count++;
                        keep = NULL;
                    }
                    SDL_UnlockMutex(P.mx);
                    av_frame_free(&keep);
                }
            }
        }
        av_packet_unref(pkt);
    }

out:
    if (fmt) avformat_close_input(&fmt);
    avcodec_free_context(&vctx);
    avcodec_free_context(&actx);
    swr_free(&swr);
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    stop_server_transcode();
}

static int worker(void *arg) {
    (void)arg;
    while (!P.quit) {
        SDL_LockMutex(P.mx);
        while (!P.quit && !P.reopen) SDL_CondWaitTimeout(P.cv, P.mx, 100);
        if (P.quit) {
            SDL_UnlockMutex(P.mx);
            break;
        }
        P.reopen = 0;
        P.state = 1;
        ring_clear();
        SDL_UnlockMutex(P.mx);
        SDL_ClearQueuedAudio(P.dev);
        new_session();
        play_once();
    }
    return 0;
}

static void request_play(double pos, int audio_only) {
    SDL_LockMutex(P.mx);
    P.start_pos = pos < 0 ? 0 : pos;
    if (next_item[0]) snprintf(P.item_id, sizeof P.item_id, "%s", next_item);
    P.audio_only = audio_only;
    P.reopen = 1;
    P.state = 1;
    SDL_CondBroadcast(P.cv);
    SDL_UnlockMutex(P.mx);
}

/* ---------------------------------------------------------------- Jellyfin steps */

static char ep_title[256];

static int pick_episode(void) {
    char path[512];
    HttpResp r;
    snprintf(path, sizeof path,
             "/Items?userId=%s&includeItemTypes=Episode&recursive=true&sortBy=Random&limit=1"
             "&isMissing=false&fields=RunTimeTicks",
             cfg.user_id);
    if (jf("GET", path, NULL, &r) != 0 || r.status != 200) {
        http_free(&r);
        return -1;
    }
    const char *items = strstr(r.body, "\"Items\":[");
    char name[128] = "", series[128] = "";
    int ok = items && json_str(items, "Id", next_item, sizeof next_item) == 0;
    if (ok) {
        json_str(items, "Name", name, sizeof name);
        json_str(items, "SeriesName", series, sizeof series);
        long long s = json_num(items, "ParentIndexNumber", 0), e = json_num(items, "IndexNumber", 0);
        snprintf(ep_title, sizeof ep_title, "%s S%02lldE%02lld %s", series, s, e, name);
        logm("episode: %s (%s)", ep_title, next_item);
    }
    http_free(&r);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------- main */

enum { S_NEED_SERVER, S_QC, S_PLAY, S_ERROR };

int main(int argc, char **argv) {
    const char *data_dir = argc > 1 ? argv[1] : ".";
    const char *log_dir = argc > 2 ? argv[2] : data_dir;
    char p[1024];
    snprintf(p, sizeof p, "%s/SleepShuffleTest.txt", log_dir);
    g_log = fopen(p, "a");
    snprintf(cfg_path, sizeof cfg_path, "%s/config.txt", data_dir);
    logm("==== Sleep Shuffle test pak %s ====", APP_VERSION);
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGFPE, on_crash);
    logm("data=%s logs=%s SDL_VIDEODRIVER=%s", data_dir, log_dir, getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "");
    av_log_set_callback(ff_log);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    cfg_load();
    if (getenv("SS_SERVER")) snprintf(cfg.server, sizeof cfg.server, "%s", getenv("SS_SERVER"));
    cfg_normalize_server();
    if (!cfg.device_id[0]) {
        for (int i = 0; i < 24; i++) cfg.device_id[i] = "0123456789abcdef"[rand() % 16];
        cfg.device_id[24] = 0;
    }
    cfg_save();

    STEP("SDL_Init");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        logm("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0) {
        SW = dm.w;
        SH = dm.h;
    }
    logm("display mode %dx%d @%dHz", dm.w, dm.h, dm.refresh_rate);
    int windowed = getenv("SS_WINDOWED") != NULL;
    STEP("create window");
    if (windowed) SW = 640, SH = 480;
    SDL_Window *win = SDL_CreateWindow("Sleep Shuffle test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SW, SH,
                                       windowed ? 0 : SDL_WINDOW_FULLSCREEN);
    if (!win) logm("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    STEP("create renderer");
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(ren, &ri);
    logm("SDL %d.%d.%d video=%s renderer=%s screen=%dx%d audio=%s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
         SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver(), ri.name, SW, SH, SDL_GetCurrentAudioDriver());
    {
        char gov[32] = "?";
        FILE *g = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
        if (g) {
            if (!fgets(gov, sizeof gov, g)) gov[0] = 0;
            fclose(g);
            gov[strcspn(gov, "\n")] = 0;
        }
        logm("cpu governor=%s min=%dMHz max=%dMHz", gov,
             read_int_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq") / 1000,
             read_int_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq") / 1000);
    }
    SDL_ShowCursor(SDL_DISABLE);
    STEP("first frame");
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);
    STEP("fonts");
    fonts_init();
    text_c(0, SH / 2 - 10, 200, "Starting...");
    SDL_RenderPresent(ren);
    STEP("controllers");

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        logm("joystick %d: %s (game controller: %s)", i, SDL_JoystickNameForIndex(i),
             SDL_IsGameController(i) ? "yes" : "no");
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
        else SDL_JoystickOpen(i);
    }

    STEP("audio");
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    P.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!P.dev) logm("audio device failed: %s", SDL_GetError());
    P.mx = SDL_CreateMutex();
    P.cv = SDL_CreateCond();
    SDL_Thread *th = SDL_CreateThread(worker, "decode", NULL);
    atexit(restore_backlight);
    g_heartbeat = SDL_GetTicks();
    SDL_DetachThread(SDL_CreateThread(watchdog, "watchdog", NULL));
    STEP("sign-in");

    int screen = S_NEED_SERVER;
    char qc_code[16] = "", qc_secret[128] = "", err[256] = "";
    Uint32 qc_next = 0, hint_until = 0, stats_next = SDL_GetTicks() + 10000, audio_since = 0;
    int show_stats = 0, running = 1, screen_off = 0;
    SDL_Texture *vtex = NULL;
    int tw = 0, th_ = 0;
    const char *autotest = getenv("SS_AUTOTEST");
    Uint32 started = SDL_GetTicks();
    long last_decoded = 0, last_dropped = 0;
    double last_decode_ms = 0;

    /* ---- sign-in */
    if (!cfg.server[0]) {
        snprintf(err, sizeof err, "Add your Jellyfin address to config.txt");
        logm("no server set in %s", cfg_path);
    } else {
        HttpResp r;
        if (jf("GET", "/System/Info/Public", NULL, &r) != 0 || r.status != 200) {
            snprintf(err, sizeof err, "Can't reach Jellyfin at %s", cfg.server);
            screen = S_ERROR;
        } else {
            char ver[32] = "";
            json_str(r.body, "Version", ver, sizeof ver);
            logm("Jellyfin %s", ver);
            screen = cfg.token[0] ? S_PLAY : S_QC;
        }
        http_free(&r);
        if (screen == S_PLAY) { /* check the saved token still works */
            char up[128];
            snprintf(up, sizeof up, "/Users/%s", cfg.user_id);
            if (jf("GET", up, NULL, &r) != 0 || r.status != 200) {
                logm("saved token rejected; signing in again");
                cfg.token[0] = 0;
                screen = S_QC;
            }
            http_free(&r);
        }
        if (screen == S_QC) {
            if (jf("POST", "/QuickConnect/Initiate", NULL, &r) != 0 || r.status != 200) {
                http_free(&r);
                jf("GET", "/QuickConnect/Initiate", NULL, &r); /* servers before 10.9 */
            }
            if (r.status == 200 && json_str(r.body, "Code", qc_code, sizeof qc_code) == 0 &&
                json_str(r.body, "Secret", qc_secret, sizeof qc_secret) == 0) {
                logm("quick connect code issued");
            } else {
                snprintf(err, sizeof err,
                         r.status == 401 ? "Quick Connect is off. Turn it on in the Jellyfin dashboard."
                                         : "Quick Connect didn't start (HTTP %d).",
                         r.status);
                screen = S_ERROR;
            }
            http_free(&r);
        }
    }
    if (screen == S_PLAY) {
        if (pick_episode() == 0) {
            request_play(0, 0);
            hint_until = SDL_GetTicks() + 6000;
        } else {
            snprintf(err, sizeof err, "Couldn't get an episode from Jellyfin.");
            screen = S_ERROR;
        }
    }

    STEP("main loop");
    while (running) {
        Uint32 now = SDL_GetTicks();
        g_heartbeat = now;
        int act_toggle = 0, act_next = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                logm("controller button %d (%s)", e.cbutton.button,
                     SDL_GameControllerGetStringForButton(e.cbutton.button));
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A: act_toggle = 1; break;
                case SDL_CONTROLLER_BUTTON_B:
                case SDL_CONTROLLER_BUTTON_START: running = 0; break;
                case SDL_CONTROLLER_BUTTON_X: show_stats = !show_stats; break;
                case SDL_CONTROLLER_BUTTON_Y: act_next = 1; break;
                default: break;
                }
                if (screen_off) { /* any button wakes the screen for a moment */
                    backlight(1);
                    screen_off = 0;
                    audio_since = now;
                }
            } else if (e.type == SDL_JOYBUTTONDOWN) {
                logm("raw joystick button %d", e.jbutton.button);
            } else if (e.type == SDL_KEYDOWN) {
                logm("key %s", SDL_GetKeyName(e.key.keysym.sym));
                if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                if (e.key.keysym.sym == SDLK_SPACE) act_toggle = 1;
                if (e.key.keysym.sym == SDLK_s) show_stats = !show_stats;
                if (e.key.keysym.sym == SDLK_n) act_next = 1;
            }
        }
        if (autotest) { /* desktop self-test: toggle at 6 s and 12 s, quit at 18 s */
            static int step = 0;
            Uint32 t = now - started;
            if (step == 0 && t > 6000 && screen == S_PLAY) act_toggle = 1, step = 1;
            else if (step == 1 && t > 12000) act_toggle = 1, step = 2;
            else if (step == 2 && t > 18000) running = 0;
            if (screen != S_PLAY && t > 8000) running = 0;
        }

        /* ---- Quick Connect polling */
        if (screen == S_QC && now >= qc_next) {
            qc_next = now + 3000;
            char path[256];
            HttpResp r;
            snprintf(path, sizeof path, "/QuickConnect/Connect?Secret=%s", qc_secret);
            if (jf("GET", path, NULL, &r) == 0 && r.status == 200 && json_bool(r.body, "Authenticated") == 1) {
                http_free(&r);
                char body[256];
                snprintf(body, sizeof body, "{\"Secret\":\"%s\"}", qc_secret);
                if (jf("POST", "/Users/AuthenticateWithQuickConnect", body, &r) == 0 && r.status == 200 &&
                    json_str(r.body, "AccessToken", cfg.token, sizeof cfg.token) == 0) {
                    const char *u = strstr(r.body, "\"User\":");
                    if (u) {
                        json_str(u, "Id", cfg.user_id, sizeof cfg.user_id);
                        json_str(u, "Name", cfg.user_name, sizeof cfg.user_name);
                    }
                    logm("signed in as %s", cfg.user_name);
                    cfg_save();
                    if (pick_episode() == 0) {
                        screen = S_PLAY;
                        request_play(0, 0);
                        hint_until = now + 6000;
                    } else {
                        snprintf(err, sizeof err, "Signed in, but couldn't get an episode.");
                        screen = S_ERROR;
                    }
                } else {
                    snprintf(err, sizeof err, "Quick Connect sign-in failed (HTTP %d).", r.status);
                    screen = S_ERROR;
                }
            }
            http_free(&r);
        }

        /* ---- actions */
        if (screen == S_PLAY && act_toggle) {
            double pos;
            SDL_LockMutex(P.mx);
            pos = P.has_audio ? clock_now() : P.start_pos;
            int to_audio = !P.audio_only;
            SDL_UnlockMutex(P.mx);
            logm("switching to %s at %.1fs", to_audio ? "audio only" : "video", pos);
            request_play(pos, to_audio);
            if (to_audio) audio_since = now;
            else if (screen_off) {
                backlight(1);
                screen_off = 0;
            }
            hint_until = now + 4000;
        }
        if (screen == S_PLAY && act_next) {
            if (pick_episode() == 0) request_play(0, P.audio_only);
            hint_until = now + 4000;
        }
        if (screen == S_PLAY && P.audio_only && !screen_off && audio_since && now - audio_since > 5000 &&
            !show_stats) {
            backlight(0);
            screen_off = 1;
        }

        /* ---- periodic stats for the log */
        if (now >= stats_next) {
            stats_next = now + 10000;
            long d = P.decoded - last_decoded, dr = P.dropped - last_dropped;
            double ms = P.decode_ms - last_decode_ms;
            last_decoded = P.decoded;
            last_dropped = P.dropped;
            last_decode_ms = P.decode_ms;
            double load = 0;
            FILE *lf = fopen("/proc/loadavg", "r");
            if (lf) {
                if (fscanf(lf, "%lf", &load) != 1) load = 0;
                fclose(lf);
            }
            logm("stats: mode=%s route=%s state=%d fps=%.1f dropped=%ld decode=%.1fms/frame audioq=%.0fms "
                 "underruns=%ld temp=%dC battery=%d%% load=%.2f cpu_mhz=%d",
                 P.audio_only ? "audio" : "video", P.route, P.state, d / 10.0, dr, d ? ms / d : 0,
                 queued_secs() * 1000, P.audio_underruns, read_int_file("/sys/class/thermal/thermal_zone0/temp") / 1000,
                 read_int_file("/sys/class/power_supply/*/capacity"), load,
                 read_int_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") / 1000);
        }

        /* ---- pick the frame to show */
        AVFrame *show = NULL;
        SDL_LockMutex(P.mx);
        double clk = P.has_audio ? clock_now() : 0;
        while (P.count) {
            int full_and_starved = P.count == RING && P.has_audio && queued_secs() < 0.05;
            if (P.has_audio && P.ring_pts[P.head] > clk + 0.01 && !full_and_starved) break;
            if (show) {
                av_frame_free(&show);
                P.dropped++;
            }
            show = P.ring[P.head];
            P.ring[P.head] = NULL;
            P.head = (P.head + 1) % RING;
            P.count--;
            SDL_CondBroadcast(P.cv);
            if (!P.has_audio) break;
        }
        SDL_UnlockMutex(P.mx);
        if (show) {
            if (!vtex || tw != show->width || th_ != show->height) {
                if (vtex) SDL_DestroyTexture(vtex);
                vtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, show->width,
                                         show->height);
                tw = show->width;
                th_ = show->height;
            }
            SDL_UpdateYUVTexture(vtex, NULL, show->data[0], show->linesize[0], show->data[1], show->linesize[1],
                                 show->data[2], show->linesize[2]);
            P.shown++;
            av_frame_free(&show);
        }

        /* ---- draw */
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (screen == S_QC) {
            text_c(0, SH / 2 - 120, 200, "Sign in with Quick Connect");
            text_c(1, SH / 2 - 60, 255, qc_code);
            text_c(0, SH / 2 + 30, 160, "On your phone, open Jellyfin > Settings >");
            text_c(0, SH / 2 + 58, 160, "Quick Connect, and enter this code.");
            pill(SH - 60, "B  Quit");
        } else if (screen == S_ERROR || (screen == S_NEED_SERVER && err[0])) {
            text_c(0, SH / 2 - 40, 230, err[0] ? err : "Something went wrong.");
            text_c(0, SH / 2, 140, "Details are in SleepShuffleTest.txt");
            pill(SH - 60, "B  Quit");
        } else if (screen == S_PLAY) {
            if (vtex && !P.audio_only && P.state == 2) {
                double sa = (double)tw / th_, da = (double)SW / SH;
                SDL_Rect dst = {0, 0, SW, SH};
                if (sa > da) dst.h = (int)(SW / sa), dst.y = (SH - dst.h) / 2;
                else dst.w = (int)(SH * sa), dst.x = (SW - dst.w) / 2;
                SDL_RenderCopy(ren, vtex, NULL, &dst);
            }
            if (P.state == 1) text_c(0, SH / 2 - 10, 180, "Loading...");
            if (P.state == 3) {
                text_c(0, SH / 2 - 30, 200, "Episode finished.");
                pill(SH / 2 + 10, "Y  Next     B  Quit");
            }
            if (P.state == 4) {
                text_c(0, SH / 2 - 30, 200, P.msg);
                pill(SH / 2 + 10, "Y  Try another     B  Quit");
            }
            if (P.audio_only && P.state == 2) {
                text_c(0, SH / 2 - 30, 150, "Audio only");
                text_c(0, SH / 2 + 2, 90, ep_title);
            }
            if (now < hint_until) {
                text(0, 16, 12, 200, ep_title);
                pill(SH - 50, P.audio_only ? "A  Video   X  Stats   Y  Next   B  Quit"
                                           : "A  Audio only   X  Stats   Y  Next   B  Quit");
            }
            if (show_stats) {
                char l[256];
                SDL_Rect bg = {8, 40, 420, 150};
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
                SDL_RenderFillRect(ren, &bg);
                snprintf(l, sizeof l, "%s", P.route);
                text(0, 16, 46, 230, l);
                snprintf(l, sizeof l, "video %s %dx%d  audio %s", P.audio_only ? "-" : P.vcodec, P.vw, P.vh,
                         P.acodec);
                text(0, 16, 72, 230, l);
                snprintf(l, sizeof l, "decoded %ld  shown %ld  dropped %ld", P.decoded, P.shown, P.dropped);
                text(0, 16, 98, 230, l);
                snprintf(l, sizeof l, "decode %.1f ms/frame  audio queue %.0f ms",
                         P.decoded ? P.decode_ms / P.decoded : 0, queued_secs() * 1000);
                text(0, 16, 124, 230, l);
                snprintf(l, sizeof l, "temp %d C  battery %d%%",
                         read_int_file("/sys/class/thermal/thermal_zone0/temp") / 1000,
                         read_int_file("/sys/class/power_supply/*/capacity"));
                text(0, 16, 150, 230, l);
            }
        }
        SDL_RenderPresent(ren);
        SDL_Delay(screen_off ? 50 : 4);
    }

    logm("quitting: decoded=%ld shown=%ld dropped=%ld underruns=%ld", P.decoded, P.shown, P.dropped,
         P.audio_underruns);
    SDL_LockMutex(P.mx);
    P.quit = 1;
    SDL_CondBroadcast(P.cv);
    SDL_UnlockMutex(P.mx);
    SDL_WaitThread(th, NULL);
    SDL_LockMutex(P.mx);
    ring_clear();
    SDL_UnlockMutex(P.mx);
    backlight(1);
    if (vtex) SDL_DestroyTexture(vtex);
    SDL_CloseAudioDevice(P.dev);
    SDL_Quit();
    if (g_log) fclose(g_log);
    return 0;
}
