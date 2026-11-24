/* (ps5, lot 86) Player 1's DualSense microphone for host/nn_audio_in.c: the ocarina played by voice (or humming), as Longshot
   does on a PC through SDL (host/hos_sdl.c). host/posix/hos_sdl_ps5.c gives this file the console's calls (sceAudioIn, the
   signed-in user, a thread, the log) and answers host/hos_sdl.h's hos_sdl_mic_* with it; tests/test_ps5_microphone.py runs it
   on the PC with stand-ins for them. SPDX-License-Identifier: GPL-3.0-or-later.

   After mihawk-99's PS5_RetroArch, src/microphone_ps5.cpp (Mihawk, GPL-3.0-or-later, commit 6f03aa22; its AudioIn calls follow
   RafaelNGP/xbox-cloud-gaming-ps5's, its header says, and were checked there on a console with firmware 13.60): the service
   started once (sceAudioInInit; "already started" is no failure), a port for the user opened with sceAudioInOpen(user, type,
   0, 256-frame grains, 48000 Hz, else 16000, param 0: S16 mono), one thread that blocks in sceAudioInInput for a grain and
   asks sceAudioInGetSilentState after it, a bounded queue whose oldest sound goes first, and the silent states that must never
   let queued speech through (1: no microphone, 4: muted by the player). And after mateo-cogeanu's ProsperoCraft notes
   (docs/client-audioin-service-177.md, client-audioin-port-178.md; GPL-3.0, notes only): there, under a game's memory, a late
   sceAudioInInit failed 0x80260108 and a late sceAudioInOpen 0x8026010b, and the service and the port made early, then kept,
   worked. What differs here, and why:
   - the port is made once, early (hos_sdl_init's controllers, after main: never in a static initializer), and kept for the
     whole run, parked while no one listens: the game starts and stops its input streams each time the ocarina comes out
     (a fraction of a second to a song's length), and a port opened late may be refused;
   - one capture thread for the whole run, asleep while no one listens: an ended thread's stack is never given back on the
     console (host/posix/win32_posix.c CreateThread);
   - readers, not one queue: each hos_sdl_mic_open is a reader with its own ring, in the format nn_audio_in.c asks (SDL
     converts there: here the port's S16 mono is resampled by linear interpolation, its channels copied, its samples written as
     S8, S16, S32 or float), and nothing blocks (hos_sdl_mic_available and hos_sdl_mic_read answer what the ring holds);
   - any silent state but 0 is silence, for the grain read before it is known as well (the state is asked after each grain:
     a grain passes only when the state was 0 both before and after it), and when the state leaves 0 every reader's ring is
     emptied: speech queued before a mute or a pulled-out microphone never reaches the game;
   - the type tried first is 1 (general), then 0 (voice chat), at 48000 Hz, then the same at 16000: the game reads the
     "true raw" device as well, and a voice-chat path may clean a steady hum away (HOOKSHOT_PS5_MIC_TYPE=0: 0 first).

   The player's voice never leaves the console: nothing here writes a file or sends anything; the log has levels, counts and
   bytes a second, never a sample. */
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOS_PS5_MIC_GRAIN 256        /* frames a sceAudioInInput gives */
#define HOS_PS5_MIC_ID 1u            /* the list's one microphone (hos_sdl_mic_list); 0, the default, is the same one */
#define HOS_PS5_MIC_NAME "DualSense microphone"
#define HOS_PS5_MIC_READERS 8        /* nn_audio_in.c opens one a source: at most its 5 */
#define HOS_PS5_MIC_RING_MAX (1 << 20)
#define HOS_PS5_MIC_MAGIC 0x4d494331u   /* "MIC1" */
/* sceAudioInInit's "already started" answers (PS5_RetroArch's) */
#define HOS_PS5_MIC_INIT_DONE_1 ((int)0x8026000e)
#define HOS_PS5_MIC_INIT_DONE_2 ((int)0x8026000c)

/* The console's calls, and what the run needs around them (host/posix/hos_sdl_ps5.c; the test's stand-ins) */
typedef struct {
    int (*init)(void);                                                         /* sceAudioInInit */
    int (*open)(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t param);   /* sceAudioInOpen */
    int (*input)(int32_t port, void *pcm);                                     /* sceAudioInInput: blocks for a grain */
    int (*silent)(int32_t port);                                               /* sceAudioInGetSilentState */
    int (*close)(int32_t port);                                                /* sceAudioInClose */
    int32_t (*user)(void);           /* player 1's user (the one who started the title): -1 none */
    int (*player)(void);             /* player 1's controller is there */
    int (*thread)(void *(*main)(void *));   /* the capture thread, started once: 0 started */
    int64_t (*now_ns)(void);
    void (*log)(const char *fmt, ...);
    const char *(*memory)(char *buf, size_t cap);   /* " (flexible N MiB free ...)" for a line of the log, or "" */
    int enabled;                     /* HOOKSHOT_PS5_MIC=1 */
    int first_type;                  /* 1, or 0 with HOOKSHOT_PS5_MIC_TYPE=0 */
} HosPs5MicOps;

/* SDL_AudioFormat's, as nn_audio_in.c asks them */
enum { HOS_PS5_MIC_S8 = 0x8008, HOS_PS5_MIC_S16 = 0x8010, HOS_PS5_MIC_S32 = 0x8020, HOS_PS5_MIC_F32 = 0x8120 };

typedef struct {
    uint32_t magic;
    int freq, channels, format, sample_bytes, frame_bytes;
    uint8_t *ring;                   /* what was captured for it and not read, in its format: the oldest dropped when full */
    int cap, start, len;
    int64_t phase;                   /* (the resampling: where the next frame falls, in input samples times freq) */
    int16_t last;                    /* (the input's sample before this grain) */
    uint64_t given, taken, dropped;  /* bytes */
    int64_t opened_ns;
} HosPs5MicReader;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t wake;             /* (the thread: a reader came, or the run ends; a shutdown: the input returned) */
    const HosPs5MicOps *ops;
    int state;                       /* 0 not tried, 1 the port open, -1 refused (tried again at the next open), -2 closed */
    int inited;                      /* (sceAudioInInit answered) */
    int init_result;
    int cond_made, thread_on, in_input, shutdown;
    int32_t port, user;
    int type;
    unsigned rate;
    HosPs5MicReader *readers[HOS_PS5_MIC_READERS];
    int nreaders;
    int silent;                      /* the state after the last grain; -1 not known (a capture's first grain) */
    int said_silent;                 /* (the state the log said last) */
    /* the capture's counts: since it started (no reader before), and for the log's line a second */
    int64_t session_ns, window_ns;
    uint64_t s_grains, s_silent, s_given, s_taken, s_dropped;
    uint64_t w_grains, w_silent, w_given, w_taken, w_dropped;
    int s_peak, w_peak, first_result, said_first;
    uint64_t captures;               /* (how many times it started: the log's number for each) */
    int draining, drained;           /* (a capture's start: the grains the port held while parked, which come at once, dropped) */
    int fast_run, paced;             /* (grains that came at once in a row; past 32 the input does not block: paced here) */
    char why[200];                   /* the last refusal, for hos_sdl_error */
} hos_ps5_mic = {.mu = PTHREAD_MUTEX_INITIALIZER};

static const char *mic_format_name(int f) {
    return f == HOS_PS5_MIC_S8 ? "S8" : f == HOS_PS5_MIC_S16 ? "S16" : f == HOS_PS5_MIC_S32 ? "S32" : f == HOS_PS5_MIC_F32 ? "float" : "?";
}
static const char *mic_silent_name(int s) {
    switch (s) {
    case 0: return "sound passes";
    case 1: return "no microphone";
    case 2: return "another application has the microphone";
    case 4: return "muted by the player";
    case 8: return "a format the microphone does not give";
    default: return "silent";
    }
}

/* ---- a reader's ring ---- */
static void mic_put(HosPs5MicReader *r, const uint8_t *p, int n) {   /* (the oldest dropped when full) */
    const int drop = r->len + n - r->cap;
    if (drop > 0) {
        r->start = (r->start + drop) % r->cap, r->len -= drop, r->dropped += (uint64_t)drop;
        hos_ps5_mic.s_dropped += (uint64_t)drop, hos_ps5_mic.w_dropped += (uint64_t)drop;
    }
    const int at = (r->start + r->len) % r->cap, first = n < r->cap - at ? n : r->cap - at;
    memcpy(r->ring + at, p, (size_t)first);
    memcpy(r->ring, p + first, (size_t)(n - first));
    r->len += n, r->given += (uint64_t)n;
    hos_ps5_mic.s_given += (uint64_t)n, hos_ps5_mic.w_given += (uint64_t)n;
}
/* one frame of the port's S16 mono, in the reader's format, each channel the same */
static void mic_frame(HosPs5MicReader *r, int v) {
    uint8_t f[8 * 4];
    for (int c = 0; c < r->channels; c++) {
        uint8_t *const at = f + c * r->sample_bytes;
        if (r->format == HOS_PS5_MIC_S8) {
            const int8_t s = (int8_t)(v >> 8);
            memcpy(at, &s, 1);
        } else if (r->format == HOS_PS5_MIC_S16) {
            const int16_t s = (int16_t)v;
            memcpy(at, &s, 2);
        } else if (r->format == HOS_PS5_MIC_S32) {
            const int32_t s = (int32_t)((uint32_t)(int32_t)v << 16);
            memcpy(at, &s, 4);
        } else {
            const float s = (float)v / 32768.0f;
            memcpy(at, &s, 4);
        }
    }
    mic_put(r, f, r->frame_bytes);
}
/* a grain of the port's (n frames at rate) to a reader: as it is at its own rate, else by linear interpolation between the
   samples around each of its frames (the one before the grain kept) */
static void mic_feed(HosPs5MicReader *r, const int16_t *in, int n, unsigned rate) {
    if ((unsigned)r->freq == rate) {
        for (int i = 0; i < n; i++) mic_frame(r, in[i]);
    } else {
        for (;;) {   /* (frame k at input sample k * rate / freq: between buf[i] and buf[i + 1], buf[0] the last, buf[j] in[j - 1]) */
            const int64_t i = r->phase / r->freq;
            if (i >= n) break;
            const int a = i == 0 ? r->last : in[i - 1], b = in[i];
            mic_frame(r, a + (int)((int64_t)(b - a) * (r->phase % r->freq) / r->freq));
            r->phase += rate;
        }
        r->phase -= (int64_t)n * r->freq;
    }
    r->last = in[n - 1];
}
static void mic_empty(HosPs5MicReader *r) { r->start = r->len = 0, r->last = 0; }

/* ---- the port: the service, then a port for player 1's user, then the thread (once); mu held ---- */
static void *mic_main(void *unused);
static int mic_prepare_locked(char *said, size_t cap) {
    const HosPs5MicOps *const o = hos_ps5_mic.ops;
    if (hos_ps5_mic.state == 1) return 1;
    if (hos_ps5_mic.state == -2) {
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "the microphone was closed (the title is closing)");
        return 0;
    }
    char before[160] = "", after[160] = "";
    if (o->memory) o->memory(before, sizeof before);
    if (!hos_ps5_mic.inited) {
        const int r = o->init();
        if (r < 0 && r != HOS_PS5_MIC_INIT_DONE_1 && r != HOS_PS5_MIC_INIT_DONE_2) {
            hos_ps5_mic.state = -1;
            snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "sceAudioInInit refused it (%#x)", (unsigned)r);
            snprintf(said, cap, "ps5: mic: %s: no microphone, tried again when the game next listens%s", hos_ps5_mic.why, before);
            return 0;
        }
        hos_ps5_mic.inited = 1, hos_ps5_mic.init_result = r;
    }
    const int32_t user = o->user();
    if (user < 0) {
        hos_ps5_mic.state = -1;
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "no signed-in user for player 1");
        snprintf(said, cap, "ps5: mic: %s: no microphone, tried again when the game next listens", hos_ps5_mic.why);
        return 0;
    }
    const int types[2] = {o->first_type ? 1 : 0, o->first_type ? 0 : 1};
    static const unsigned rates[2] = {48000, 16000};
    char tried[160] = "";
    int32_t port = -1;
    int type = -1;
    unsigned rate = 0;
    for (int k = 0; k < 4 && port < 0; k++) {
        type = types[k % 2], rate = rates[k / 2];
        port = o->open(user, type, 0, HOS_PS5_MIC_GRAIN, rate, 0);
        if (port < 0) {
            const size_t at = strlen(tried);
            snprintf(tried + at, sizeof tried - at, "%stype %d %u Hz %#x", at ? ", " : "", type, rate, (unsigned)port);
        }
    }
    if (port < 0) {
        hos_ps5_mic.state = -1;
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "sceAudioInOpen refused player 1's microphone (user %#x: %s)",
                 (unsigned)user, tried);
        snprintf(said, cap, "ps5: mic: %s: no microphone, tried again when the game next listens%s", hos_ps5_mic.why, before);
        return 0;
    }
    if (!hos_ps5_mic.thread_on) {
        if (!hos_ps5_mic.cond_made) hos_ps5_mic.cond_made = pthread_cond_init(&hos_ps5_mic.wake, NULL) == 0;
        if (!hos_ps5_mic.cond_made || o->thread(mic_main)) {
            const int closed = o->close(port);
            hos_ps5_mic.state = -1;
            snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "no capture thread (the port %#x closed again: %#x)",
                     (unsigned)port, (unsigned)closed);
            snprintf(said, cap, "ps5: mic: %s", hos_ps5_mic.why);
            return 0;
        }
        hos_ps5_mic.thread_on = 1;
    }
    hos_ps5_mic.port = port, hos_ps5_mic.user = user, hos_ps5_mic.type = type, hos_ps5_mic.rate = rate;
    hos_ps5_mic.state = 1, hos_ps5_mic.why[0] = 0;
    if (o->memory) o->memory(after, sizeof after);
    snprintf(said, cap, "ps5: mic: player 1's microphone ready (user %#x): port %#x, type %d (%s), %u Hz, S16 mono, %d-frame grains; "
             "sceAudioInInit %#x%s%s; parked (no sound read) until the game listens. Memory before%s, after%s",
             (unsigned)user, (unsigned)port, type, type ? "general" : "voice chat", rate, HOS_PS5_MIC_GRAIN,
             (unsigned)hos_ps5_mic.init_result, tried[0] ? "; refused first: " : "", tried, before, after);
    return 1;
}
/* At the start (hos_sdl_init, after main, with the controllers: player 1's user known): the port made before the game's
   memory is taken, as ProsperoCraft found it must be. Nothing with the switch off */
static void hos_ps5_mic_prepare(const HosPs5MicOps *o) {
    if (!o->enabled) {
        o->log("ps5: mic: off (HOOKSHOT_PS5_MIC=1 lets the ocarina hear player 1's DualSense microphone): the game hears silence");
        return;
    }
    char said[600] = "";
    pthread_mutex_lock(&hos_ps5_mic.mu);
    hos_ps5_mic.ops = o;
    if (hos_ps5_mic.state == 0) mic_prepare_locked(said, sizeof said);
    pthread_mutex_unlock(&hos_ps5_mic.mu);
    if (said[0]) o->log("%s", said);
}

/* ---- the capture thread: one for the run; asleep while no one listens ---- */
static void mic_line(char *line, size_t cap, const char *what, uint64_t grains, uint64_t silent, uint64_t given, uint64_t taken,
                     uint64_t dropped, int peak) {
    snprintf(line, cap, "ps5: mic: %s: %llu grains (%llu silent), %llu bytes captured, %llu read by the game, %llu dropped, "
             "peak %d of 32767 (%d reader%s)", what, (unsigned long long)grains, (unsigned long long)silent,
             (unsigned long long)given, (unsigned long long)taken, (unsigned long long)dropped, peak, hos_ps5_mic.nreaders,
             hos_ps5_mic.nreaders == 1 ? "" : "s");
}
/* a grain read (mu held): the silence rule, then to every reader; the log's lines into said (empty: none) */
static void mic_grain(int16_t *pcm, int frames, int state, char *said, size_t cap) {
    const int64_t now = hos_ps5_mic.ops->now_ns();
    const int quiet = state != 0 || hos_ps5_mic.silent != 0;   /* (the state before this grain and after it) */
    size_t at = 0;
    said[0] = 0;
    if (!hos_ps5_mic.said_first) {
        hos_ps5_mic.said_first = 1;
        at += (size_t)snprintf(said + at, cap - at, "ps5: mic: capture %llu: the first grain: sceAudioInInput %d, silent state %#x (%s); "
                               "%d grain%s the port held while parked dropped", (unsigned long long)hos_ps5_mic.captures,
                               hos_ps5_mic.first_result, (unsigned)state, mic_silent_name(state), hos_ps5_mic.drained,
                               hos_ps5_mic.drained == 1 ? "" : "s");
        hos_ps5_mic.said_silent = state;
    } else if (state != hos_ps5_mic.said_silent && at < cap) {
        at += (size_t)snprintf(said + at, cap - at, "ps5: mic: silent state %#x (%s)%s", (unsigned)state, mic_silent_name(state),
                               state ? ": silence given, what was queued dropped" : ": sound passes again after one silent grain");
        hos_ps5_mic.said_silent = state;
    }
    if (state != 0 && hos_ps5_mic.silent == 0)   /* (speech queued before a mute or a pulled-out microphone: never given) */
        for (int i = 0; i < hos_ps5_mic.nreaders; i++) mic_empty(hos_ps5_mic.readers[i]);
    hos_ps5_mic.silent = state;
    int peak = 0;
    if (quiet) memset(pcm, 0, (size_t)frames * sizeof *pcm);
    else
        for (int i = 0; i < frames; i++) {
            const int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (v > peak) peak = v;
        }
    for (int i = 0; i < hos_ps5_mic.nreaders; i++) mic_feed(hos_ps5_mic.readers[i], pcm, frames, hos_ps5_mic.rate);
    hos_ps5_mic.s_grains++, hos_ps5_mic.w_grains++;
    hos_ps5_mic.s_silent += (uint64_t)quiet, hos_ps5_mic.w_silent += (uint64_t)quiet;
    if (peak > hos_ps5_mic.s_peak) hos_ps5_mic.s_peak = peak > 32767 ? 32767 : peak;
    if (peak > hos_ps5_mic.w_peak) hos_ps5_mic.w_peak = peak > 32767 ? 32767 : peak;
    if (now - hos_ps5_mic.window_ns >= 1000000000LL) {   /* (a line a second while the game listens) */
        if (at && at + 1 < cap) said[at++] = '\n', said[at] = 0;
        if (at < cap)
            mic_line(said + at, cap - at, "the last second", hos_ps5_mic.w_grains, hos_ps5_mic.w_silent, hos_ps5_mic.w_given,
                     hos_ps5_mic.w_taken, hos_ps5_mic.w_dropped, hos_ps5_mic.w_peak);
        hos_ps5_mic.window_ns = now;
        hos_ps5_mic.w_grains = hos_ps5_mic.w_silent = hos_ps5_mic.w_given = hos_ps5_mic.w_taken = hos_ps5_mic.w_dropped = 0;
        hos_ps5_mic.w_peak = 0;
    }
}
static void mic_say(const char *said) {   /* (each line of said, outside mu) */
    char line[600];
    for (const char *p = said; p && *p;) {
        const char *end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)n, p);
        hos_ps5_mic.ops->log("%s", line);
        p = end ? end + 1 : NULL;
    }
}
static void *mic_main(void *unused) {
    (void)unused;
    int16_t pcm[HOS_PS5_MIC_GRAIN];
    char said[1200];
    pthread_mutex_lock(&hos_ps5_mic.mu);
    const HosPs5MicOps *const o = hos_ps5_mic.ops;
    for (;;) {
        while (!hos_ps5_mic.shutdown && (!hos_ps5_mic.nreaders || hos_ps5_mic.state != 1))
            pthread_cond_wait(&hos_ps5_mic.wake, &hos_ps5_mic.mu);
        if (hos_ps5_mic.shutdown) break;
        const int32_t port = hos_ps5_mic.port;
        hos_ps5_mic.in_input = 1;
        pthread_mutex_unlock(&hos_ps5_mic.mu);
        const int64_t t0 = o->now_ns();
        const int r = o->input(port, pcm);   /* (blocks for a grain: what paces the thread) */
        const int state = r >= 0 ? o->silent(port) : -1;
        const int64_t took = o->now_ns() - t0;
        pthread_mutex_lock(&hos_ps5_mic.mu);
        hos_ps5_mic.in_input = 0;
        pthread_cond_broadcast(&hos_ps5_mic.wake);   /* (a shutdown waits for the input to return) */
        if (hos_ps5_mic.shutdown) break;
        if (r >= 0 && state >= 0 && hos_ps5_mic.draining) {   /* (sound from before this capture, held while parked: not given) */
            if (took < 1000000 && hos_ps5_mic.drained < 64) {
                hos_ps5_mic.drained++;
                continue;
            }
            hos_ps5_mic.draining = 0;
        }
        if (!hos_ps5_mic.said_first) hos_ps5_mic.first_result = r;
        if (r < 0 || state < 0) {   /* (the port is lost: closed, and made again at the game's next open) */
            const int closed = o->close(port);
            hos_ps5_mic.state = -1, hos_ps5_mic.port = -1;
            snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "the microphone stopped (sceAudioInInput %#x, silent state %#x)",
                     (unsigned)r, (unsigned)state);
            snprintf(said, sizeof said, "ps5: mic: %s: the port %#x closed (%#x); made again when the game next listens",
                     hos_ps5_mic.why, (unsigned)port, (unsigned)closed);
            pthread_mutex_unlock(&hos_ps5_mic.mu);
            o->log("%s", said);
            pthread_mutex_lock(&hos_ps5_mic.mu);
            continue;
        }
        if (!hos_ps5_mic.nreaders) continue;   /* (the last one went while this grain was read) */
        const int frames = r > 0 && r < HOS_PS5_MIC_GRAIN ? r : HOS_PS5_MIC_GRAIN;
        mic_grain(pcm, frames, state, said, sizeof said);
        /* (an input that never blocks would spin and flood the rings: past 32 grains in a row that came at once, each grain is
           followed by its own length of sleep, until one blocks again) */
        if (took < 500000) {
            if (++hos_ps5_mic.fast_run >= 32 && !hos_ps5_mic.paced) {
                hos_ps5_mic.paced = 1;
                const size_t at = strlen(said);
                snprintf(said + at, sizeof said - at, "%sps5: mic: sceAudioInInput does not block (32 grains in a row came at once): "
                         "paced here, a grain's length of sleep after each", at ? "\n" : "");
            }
        } else
            hos_ps5_mic.fast_run = 0, hos_ps5_mic.paced = 0;
        const int pace = hos_ps5_mic.paced;
        const unsigned rate = hos_ps5_mic.rate;
        if (said[0] || pace) {
            pthread_mutex_unlock(&hos_ps5_mic.mu);
            if (said[0]) mic_say(said);
            if (pace) {
                const struct timespec t = {0, (long)((int64_t)frames * 1000000000LL / (rate ? rate : 48000))};
                nanosleep(&t, NULL);
            }
            pthread_mutex_lock(&hos_ps5_mic.mu);
        }
    }
    hos_ps5_mic.thread_on = 0;
    pthread_cond_broadcast(&hos_ps5_mic.wake);
    pthread_mutex_unlock(&hos_ps5_mic.mu);
    return NULL;
}

/* ---- host/hos_sdl.h's microphones ---- */
static int hos_ps5_mic_list(const HosPs5MicOps *o, uint32_t *ids, char (*names)[128], int max) {
    if (!o->enabled || max <= 0 || !o->player()) return 0;
    ids[0] = HOS_PS5_MIC_ID;
    snprintf(names[0], 128, "%s", HOS_PS5_MIC_NAME);
    return 1;
}
static const char *hos_ps5_mic_error(void) { return hos_ps5_mic.why; }
static void *hos_ps5_mic_open(const HosPs5MicOps *o, uint32_t id, int freq, int channels, uint16_t format) {
    char said[600] = "";
    if (!o->enabled) {
        pthread_mutex_lock(&hos_ps5_mic.mu);
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "no microphone on the console without HOOKSHOT_PS5_MIC=1");
        pthread_mutex_unlock(&hos_ps5_mic.mu);
        return NULL;
    }
    const int sample = format == HOS_PS5_MIC_S8 ? 1 : format == HOS_PS5_MIC_S16 ? 2 : format == HOS_PS5_MIC_S32 ||
                       format == HOS_PS5_MIC_F32 ? 4 : 0;
    pthread_mutex_lock(&hos_ps5_mic.mu);
    hos_ps5_mic.ops = o;
    HosPs5MicReader *r = NULL;
    if (id != 0 && id != HOS_PS5_MIC_ID)
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "no microphone of id %u on the console", (unsigned)id);
    else if (!sample || freq < 8000 || freq > 192000 || channels < 1 || channels > 8)
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "a format the console's microphone is not given in (%d Hz, %d channels, %#x)",
                 freq, channels, (unsigned)format);
    else if (hos_ps5_mic.nreaders == HOS_PS5_MIC_READERS)
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "too many readers of the microphone");
    else if (mic_prepare_locked(said, sizeof said) && (r = (HosPs5MicReader *)calloc(1, sizeof *r)) != NULL) {
        r->magic = HOS_PS5_MIC_MAGIC, r->freq = freq, r->channels = channels, r->format = format, r->sample_bytes = sample;
        r->frame_bytes = sample * channels;
        int cap = freq * r->frame_bytes;   /* (a second of it, at most 1 MiB, in whole frames) */
        if (cap > HOS_PS5_MIC_RING_MAX) cap = HOS_PS5_MIC_RING_MAX;
        r->cap = cap - cap % r->frame_bytes;
        r->opened_ns = o->now_ns();
        if (!(r->ring = (uint8_t *)malloc((size_t)r->cap))) {
            free(r), r = NULL;
            snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "no memory for the microphone's ring");
        } else {
            if (!hos_ps5_mic.nreaders) {   /* (a capture starts: its counts from zero, the first grain silent) */
                hos_ps5_mic.captures++;
                hos_ps5_mic.session_ns = hos_ps5_mic.window_ns = r->opened_ns;
                hos_ps5_mic.s_grains = hos_ps5_mic.s_silent = hos_ps5_mic.s_given = hos_ps5_mic.s_taken = hos_ps5_mic.s_dropped = 0;
                hos_ps5_mic.w_grains = hos_ps5_mic.w_silent = hos_ps5_mic.w_given = hos_ps5_mic.w_taken = hos_ps5_mic.w_dropped = 0;
                hos_ps5_mic.s_peak = hos_ps5_mic.w_peak = 0, hos_ps5_mic.said_first = 0, hos_ps5_mic.silent = -1;
                hos_ps5_mic.draining = 1, hos_ps5_mic.drained = 0, hos_ps5_mic.fast_run = 0, hos_ps5_mic.paced = 0;
            }
            hos_ps5_mic.readers[hos_ps5_mic.nreaders++] = r;
            pthread_cond_broadcast(&hos_ps5_mic.wake);
            const size_t at = strlen(said);
            snprintf(said + at, sizeof said - at, "%sps5: mic: capture %llu: a reader opened (%d Hz, %d channel%s, %s; the port's %u Hz%s), "
                     "%d reading", at ? "\n" : "", (unsigned long long)hos_ps5_mic.captures, freq, channels, channels == 1 ? "" : "s",
                     mic_format_name(format), hos_ps5_mic.rate, (unsigned)freq == hos_ps5_mic.rate ? "" : ", resampled",
                     hos_ps5_mic.nreaders);
        }
    } else if (hos_ps5_mic.state == 1)
        snprintf(hos_ps5_mic.why, sizeof hos_ps5_mic.why, "no memory for a microphone reader");
    pthread_mutex_unlock(&hos_ps5_mic.mu);
    if (said[0]) mic_say(said);
    return r;
}
static HosPs5MicReader *mic_reader(void *mic) {
    HosPs5MicReader *const r = (HosPs5MicReader *)mic;
    return r && r->magic == HOS_PS5_MIC_MAGIC ? r : NULL;
}
static int hos_ps5_mic_available(void *mic) {
    HosPs5MicReader *const r = mic_reader(mic);
    if (!r) return 0;
    pthread_mutex_lock(&hos_ps5_mic.mu);
    const int n = r->len;
    pthread_mutex_unlock(&hos_ps5_mic.mu);
    return n;
}
static int hos_ps5_mic_read(void *mic, void *dst, int bytes) {   /* (whole frames) */
    HosPs5MicReader *const r = mic_reader(mic);
    if (!r || !dst || bytes <= 0) return 0;
    pthread_mutex_lock(&hos_ps5_mic.mu);
    int n = bytes < r->len ? bytes : r->len;
    n -= n % r->frame_bytes;
    const int first = n < r->cap - r->start ? n : r->cap - r->start;
    memcpy(dst, r->ring + r->start, (size_t)first);
    memcpy((uint8_t *)dst + first, r->ring, (size_t)(n - first));
    r->start = (r->start + n) % r->cap, r->len -= n, r->taken += (uint64_t)n;
    hos_ps5_mic.s_taken += (uint64_t)n, hos_ps5_mic.w_taken += (uint64_t)n;
    pthread_mutex_unlock(&hos_ps5_mic.mu);
    return n;
}
static void hos_ps5_mic_clear(void *mic) {
    HosPs5MicReader *const r = mic_reader(mic);
    if (!r) return;
    pthread_mutex_lock(&hos_ps5_mic.mu);
    r->start = r->len = 0;
    pthread_mutex_unlock(&hos_ps5_mic.mu);
}
/* A reader closed: out of the list (the thread touches readers under mu only), then freed. The last one: the capture pauses,
   the port stays open, parked, for the game's next open */
static void hos_ps5_mic_close(void *mic) {
    HosPs5MicReader *const r = mic_reader(mic);
    if (!r) return;
    char said[400] = "";
    pthread_mutex_lock(&hos_ps5_mic.mu);
    for (int i = 0; i < hos_ps5_mic.nreaders; i++)
        if (hos_ps5_mic.readers[i] == r) {
            hos_ps5_mic.readers[i] = hos_ps5_mic.readers[--hos_ps5_mic.nreaders];
            hos_ps5_mic.readers[hos_ps5_mic.nreaders] = NULL;
            break;
        }
    if (!hos_ps5_mic.nreaders) {
        char what[96];
        snprintf(what, sizeof what, "capture %llu paused after %.1f s (no reader; the port stays open, parked)",
                 (unsigned long long)hos_ps5_mic.captures, (double)(hos_ps5_mic.ops->now_ns() - hos_ps5_mic.session_ns) / 1e9);
        mic_line(said, sizeof said, what, hos_ps5_mic.s_grains, hos_ps5_mic.s_silent, hos_ps5_mic.s_given, hos_ps5_mic.s_taken,
                 hos_ps5_mic.s_dropped, hos_ps5_mic.s_peak);
    }
    r->magic = 0;
    pthread_mutex_unlock(&hos_ps5_mic.mu);
    free(r->ring);
    free(r);
    if (said[0]) hos_ps5_mic.ops->log("%s", said);
}
/* The title closing (the menu's Quit, Back to the Grapnel screen: host/posix/ps5_relaunch.c): the thread out of its input
   (a grain, 5 to 16 ms; 300 ms at most), the port closed, the thread ended. Readers left get nothing more */
static void hos_ps5_mic_shutdown(void) {
    char said[300] = "";
    pthread_mutex_lock(&hos_ps5_mic.mu);
    const HosPs5MicOps *const o = hos_ps5_mic.ops;
    if (!o || (!hos_ps5_mic.thread_on && hos_ps5_mic.state != 1)) {
        hos_ps5_mic.state = -2;
        pthread_mutex_unlock(&hos_ps5_mic.mu);
        return;
    }
    hos_ps5_mic.shutdown = 1;
    if (hos_ps5_mic.cond_made) pthread_cond_broadcast(&hos_ps5_mic.wake);
    for (int i = 0; i < 60 && hos_ps5_mic.in_input; i++) {
        pthread_mutex_unlock(&hos_ps5_mic.mu);
        const struct timespec t = {0, 5000000};
        nanosleep(&t, NULL);
        pthread_mutex_lock(&hos_ps5_mic.mu);
    }
    if (hos_ps5_mic.state == 1 && !hos_ps5_mic.in_input) {
        const int closed = o->close(hos_ps5_mic.port);
        snprintf(said, sizeof said, "ps5: mic: closed: the port %#x (%#x), after %llu captures", (unsigned)hos_ps5_mic.port,
                 (unsigned)closed, (unsigned long long)hos_ps5_mic.captures);
        hos_ps5_mic.port = -1;
    } else if (hos_ps5_mic.state == 1) {
        snprintf(said, sizeof said, "ps5: mic: closed: the port %#x left to the system (its input did not return in 300 ms)",
                 (unsigned)hos_ps5_mic.port);
    }
    hos_ps5_mic.state = -2;
    pthread_mutex_unlock(&hos_ps5_mic.mu);
    if (said[0]) o->log("%s", said);
}
