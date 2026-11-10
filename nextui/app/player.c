/* Decoder thread: Jellyfin stream -> FFmpeg -> SDL audio queue + a small ring of YUV frames.
 * Audio is the clock. Proven on the RG SP by the test pak (nextui/spike): 24 fps, no drops. */
#define _GNU_SOURCE
#include "player.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#define RING 8
#define OUT_RATE 48000
#define MAX_AHEAD 0.75 /* seconds of audio queued ahead; keeps the fade and switches responsive */

static struct {
    SDL_mutex *mx;
    SDL_cond *cv;
    SDL_Thread *th;
    SDL_AudioDeviceID dev;
    int quit, reopen, stop;
    char item[48];
    double start;
    int audio_only, sw, sh;
    char session[40];
    PlayerState state;
    char err[200], route[64];
    AVFrame *ring[RING];
    double ring_pts[RING];
    int head, count;
    int has_audio;
    double audio_end;
    double wall_base;
    volatile float gain;
} P;

static double queued(void) { return SDL_GetQueuedAudioSize(P.dev) / (double)(OUT_RATE * 4); }
static double clock_now(void) {
    return P.has_audio ? P.audio_end - queued() : av_gettime_relative() / 1e6 - P.wall_base;
}

static int interrupt_cb(void *o) {
    (void)o;
    return P.quit || P.reopen || P.stop;
}

static void ring_clear(void) {
    while (P.count) {
        av_frame_free(&P.ring[P.head]);
        P.head = (P.head + 1) % RING;
        P.count--;
    }
}

static void new_session(void) {
    for (int i = 0; i < 32; i++) P.session[i] = "0123456789abcdef"[rand() % 16];
    P.session[32] = 0;
}

/* route 0: progressive TS from the start position (video), 1: HLS + seek (video fallback),
 * 2: MP3 from the start position (audio only), 3: progressive TS with video discarded (audio fallback) */
static void make_url(char *url, size_t n, int route) {
    long long ticks = (long long)(P.start * 1e7);
    const char *b = g_base;
    switch (route) {
    case 0:
    case 3:
        snprintf(url, n,
                 "%s/Videos/%s/stream.ts?container=ts&mediaSourceId=%s&deviceId=%s&playSessionId=%s&static=false"
                 "&videoCodec=h264&audioCodec=aac&audioChannels=2&maxWidth=%d&maxHeight=%d"
                 "&videoBitRate=1200000&audioBitRate=128000&startTimeTicks=%lld&ApiKey=%s",
                 b, P.item, P.item, cfg.device_id, P.session, P.sw, P.sh, ticks, cfg.token);
        break;
    case 1:
        snprintf(url, n,
                 "%s/Videos/%s/master.m3u8?mediaSourceId=%s&deviceId=%s&playSessionId=%s"
                 "&videoCodec=h264&audioCodec=aac&maxAudioChannels=2&maxWidth=%d&maxHeight=%d"
                 "&videoBitrate=1200000&audioBitrate=128000&segmentContainer=ts&ApiKey=%s",
                 b, P.item, P.item, cfg.device_id, P.session, P.sw, P.sh, cfg.token);
        break;
    default:
        snprintf(url, n,
                 "%s/Audio/%s/stream.mp3?container=mp3&audioCodec=mp3&audioBitRate=128000&audioChannels=2"
                 "&deviceId=%s&playSessionId=%s&startTimeTicks=%lld&ApiKey=%s",
                 b, P.item, cfg.device_id, P.session, ticks, cfg.token);
    }
}

static const char *route_name(int r) {
    static const char *names[] = {"video", "video (HLS)", "audio only", "audio only (from video stream)"};
    return names[r];
}

static void apply_gain(int16_t *s, int count, float g) {
    if (g >= 0.999f) return;
    for (int i = 0; i < count; i++) s[i] = (int16_t)(s[i] * g);
}

static void play_once(void) {
    double start = P.start;
    int audio_only = P.audio_only;
    AVFormatContext *fmt = NULL;
    AVCodecContext *vctx = NULL, *actx = NULL;
    SwrContext *swr = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int vi = -1, ai = -1, route = -1;
    double offset = 0;
    char url[2048];
    int routes[2] = {audio_only ? 2 : 0, audio_only ? 3 : 1};

    for (int a = 0; a < 2 && route < 0; a++) {
        int r = routes[a];
        make_url(url, sizeof url, r);
        logm("opening %s at %.1fs", route_name(r), start);
        fmt = avformat_alloc_context();
        fmt->interrupt_callback.callback = interrupt_cb;
        AVDictionary *o = NULL;
        char auth[512], hdr[600];
        auth_header(auth, sizeof auth);
        snprintf(hdr, sizeof hdr, "Authorization: %s\r\n", auth);
        av_dict_set(&o, "headers", hdr, 0);
        av_dict_set(&o, "rw_timeout", "20000000", 0);
        av_dict_set(&o, "reconnect", "1", 0);
        av_dict_set(&o, "probesize", "1000000", 0);
        av_dict_set(&o, "analyzeduration", "3000000", 0);
        Uint32 t0 = SDL_GetTicks();
        int rc = avformat_open_input(&fmt, url, NULL, &o);
        av_dict_free(&o);
        if (rc < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
            char e[128];
            av_strerror(rc, e, sizeof e);
            logm("open failed: %s (%u ms)", e, SDL_GetTicks() - t0);
            avformat_close_input(&fmt);
            if (P.quit || P.reopen || P.stop) goto out;
            continue;
        }
        logm("opened %s in %u ms", fmt->iformat->name, SDL_GetTicks() - t0);
        route = r;
        if (r == 1) {
            if (start > 1.0)
                avformat_seek_file(fmt, -1, INT64_MIN, (int64_t)(start * AV_TIME_BASE), INT64_MAX, 0);
        } else {
            /* these streams start at `start`; their timestamps begin near zero (or at the
             * container's start_time), so shift them onto the episode's timeline */
            double st = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time / (double)AV_TIME_BASE : 0;
            offset = start - st;
        }
    }
    if (route < 0) {
        snprintf(P.err, sizeof P.err, "Jellyfin wouldn't send this episode.");
        P.state = PL_ERROR;
        goto out;
    }
    snprintf(P.route, sizeof P.route, "%s", route_name(route));

    ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (!audio_only) vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if ((int)i != vi && (int)i != ai) fmt->streams[i]->discard = AVDISCARD_ALL;
    if (vi >= 0) {
        const AVCodec *c = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
        vctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(vctx, fmt->streams[vi]->codecpar);
        vctx->thread_count = 4;
        vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (!c || avcodec_open2(vctx, c, NULL) < 0) {
            avcodec_free_context(&vctx);
            vi = -1;
        } else
            logm("video %dx%d", vctx->width, vctx->height);
    }
    if (ai >= 0) {
        const AVCodec *c = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
        actx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(actx, fmt->streams[ai]->codecpar);
        if (!c || avcodec_open2(actx, c, NULL) < 0) {
            avcodec_free_context(&actx);
            ai = -1;
        } else {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, OUT_RATE, &actx->ch_layout, actx->sample_fmt,
                                actx->sample_rate, 0, NULL);
            swr_init(swr);
        }
    }

    SDL_LockMutex(P.mx);
    P.has_audio = ai >= 0;
    P.audio_end = start;
    P.wall_base = av_gettime_relative() / 1e6 - start;
    P.state = PL_PLAYING;
    SDL_UnlockMutex(P.mx);
    SDL_ClearQueuedAudio(P.dev);
    SDL_PauseAudioDevice(P.dev, 0);
    int first_audio = 1;

    while (!P.quit && !P.reopen && !P.stop) {
        if (P.has_audio && queued() > MAX_AHEAD) {
            SDL_Delay(10);
            continue;
        }
        int rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            if (rc == AVERROR_EOF || (fmt->pb && avio_feof(fmt->pb))) {
                while (!P.quit && !P.reopen && !P.stop && P.has_audio && queued() > 0.05) SDL_Delay(50);
                if (!P.quit && !P.reopen && !P.stop) {
                    logm("end of episode at %.1fs", clock_now());
                    P.state = PL_ENDED;
                }
            } else if (!P.quit && !P.reopen && !P.stop) {
                char e[128];
                av_strerror(rc, e, sizeof e);
                logm("read error: %s", e);
                snprintf(P.err, sizeof P.err, "The stream dropped. Check the Wi-Fi.");
                P.state = PL_ERROR;
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
                        apply_gain((int16_t *)buf, n * 2, P.gain);
                        SDL_QueueAudio(P.dev, buf, (Uint32)n * 4);
                        SDL_LockMutex(P.mx);
                        if (first_audio && pts >= 0) P.audio_end = pts;
                        first_audio = 0;
                        P.audio_end += n / (double)OUT_RATE;
                        SDL_UnlockMutex(P.mx);
                    }
                    av_freep(&buf);
                }
            }
        } else if (pkt->stream_index == vi && vctx) {
            if (avcodec_send_packet(vctx, pkt) == 0) {
                while (avcodec_receive_frame(vctx, fr) == 0) {
                    int64_t ts = fr->best_effort_timestamp;
                    double pts = ts == AV_NOPTS_VALUE ? 0 : ts * av_q2d(st->time_base) + offset;
                    if (pts < clock_now() - 0.12) {
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
                    } else
                        av_frame_move_ref(keep, fr);
                    av_frame_unref(fr);
                    SDL_LockMutex(P.mx);
                    while (P.count == RING && !P.quit && !P.reopen && !P.stop) SDL_CondWaitTimeout(P.cv, P.mx, 50);
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
    jf_stop_encoding(P.session);
}

static int worker(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(P.mx);
        while (!P.quit && !P.reopen) SDL_CondWaitTimeout(P.cv, P.mx, 100);
        if (P.quit) {
            SDL_UnlockMutex(P.mx);
            break;
        }
        P.reopen = 0; /* P.stop is cleared only by player_play, so a stop sent right after a play still wins */
        if (P.stop) {
            SDL_UnlockMutex(P.mx);
            continue;
        }
        P.state = PL_OPENING;
        ring_clear();
        SDL_UnlockMutex(P.mx);
        SDL_ClearQueuedAudio(P.dev);
        new_session();
        play_once();
        if (P.stop) {
            SDL_ClearQueuedAudio(P.dev);
            SDL_LockMutex(P.mx);
            ring_clear();
            if (P.state != PL_ERROR) P.state = PL_IDLE;
            SDL_UnlockMutex(P.mx);
        }
    }
    return 0;
}

static void ff_log(void *avcl, int level, const char *fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_ERROR) return;
    char line[400];
    vsnprintf(line, sizeof line, fmt, vl);
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = 0;
    logm("ffmpeg: %s", line);
}

int player_init(void) {
    av_log_set_callback(ff_log);
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    P.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!P.dev) logm("audio device failed: %s", SDL_GetError());
    P.gain = 1.0f;
    P.mx = SDL_CreateMutex();
    P.cv = SDL_CreateCond();
    P.th = SDL_CreateThread(worker, "decoder", NULL);
    return P.dev ? 0 : -1;
}

void player_shutdown(void) {
    SDL_LockMutex(P.mx);
    P.quit = 1;
    SDL_CondBroadcast(P.cv);
    SDL_UnlockMutex(P.mx);
    SDL_WaitThread(P.th, NULL);
    ring_clear();
    if (P.dev) SDL_CloseAudioDevice(P.dev);
}

void player_play(const char *item_id, double pos, int audio_only, int sw, int sh) {
    SDL_LockMutex(P.mx);
    snprintf(P.item, sizeof P.item, "%s", item_id);
    P.start = pos < 0 ? 0 : pos;
    P.audio_only = audio_only;
    P.sw = sw;
    P.sh = sh;
    P.reopen = 1;
    P.stop = 0;
    P.state = PL_OPENING;
    P.err[0] = 0;
    SDL_CondBroadcast(P.cv);
    SDL_UnlockMutex(P.mx);
}

void player_stop(void) {
    SDL_LockMutex(P.mx);
    P.stop = 1;
    P.reopen = 0;
    if (P.state == PL_OPENING || P.state == PL_PLAYING) P.state = PL_IDLE;
    SDL_CondBroadcast(P.cv);
    SDL_UnlockMutex(P.mx);
    SDL_ClearQueuedAudio(P.dev);
}

PlayerState player_state(void) { return P.state; }
const char *player_error(void) { return P.err; }
int player_audio_only(void) { return P.audio_only; }
const char *player_route(void) { return P.route; }
void player_set_gain(float g) { P.gain = g < 0 ? 0 : g > 1 ? 1 : g; }

double player_position(void) {
    SDL_LockMutex(P.mx);
    double t = P.state == PL_PLAYING ? clock_now() : P.start;
    SDL_UnlockMutex(P.mx);
    return t < 0 ? 0 : t;
}

VideoFrame *player_take_frame(void) {
    AVFrame *show = NULL;
    SDL_LockMutex(P.mx);
    double clk = clock_now();
    while (P.count) {
        int starved = P.count == RING && P.has_audio && queued() < 0.05;
        if (P.ring_pts[P.head] > clk + 0.01 && !starved) break;
        if (show) av_frame_free(&show);
        show = P.ring[P.head];
        P.ring[P.head] = NULL;
        P.head = (P.head + 1) % RING;
        P.count--;
        SDL_CondBroadcast(P.cv);
        if (!P.has_audio) break;
    }
    SDL_UnlockMutex(P.mx);
    if (!show) return NULL;
    VideoFrame *v = calloc(1, sizeof *v);
    v->w = show->width;
    v->h = show->height;
    for (int i = 0; i < 3; i++) {
        v->data[i] = show->data[i];
        v->linesize[i] = show->linesize[i];
    }
    v->priv = show;
    return v;
}

void player_frame_free(VideoFrame *f) {
    if (!f) return;
    AVFrame *a = f->priv;
    av_frame_free(&a);
    free(f);
}
